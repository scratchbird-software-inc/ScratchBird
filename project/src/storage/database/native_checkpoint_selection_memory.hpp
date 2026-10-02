// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_checkpoint_selection.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <limits>
#include <system_error>

namespace scratchbird::storage::database {
enum class NativeCheckpointSelectionMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  bootstrap_failure, encrypted_requires_authority, header_failure,
  selection_failure, object_mismatch, io_failure, resource_exhausted, lock_failure
};
struct NativeCheckpointSelectionMemoryResult {
  NativeCheckpointSelectionMemoryError error=NativeCheckpointSelectionMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
  NativeCheckpointSelectionError selection_error=NativeCheckpointSelectionError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  u64 page_bytes_read=0;
  std::optional<NativeCheckpointSelection> selection;
  NativeStorageBuffer image;
  bool ok() const noexcept {
    return error==NativeCheckpointSelectionMemoryError::none&&selection.has_value()&&bool(image);
  }
};

// STORAGE-NATIVE-SELECTOR-MEMORY-001. A single inspected image, not a selected
// checkpoint or serving grant. The target is NOT assumed to be a current root.
// The owning node's actual manager/ledger must outlive the returned page owner.
inline NativeCheckpointSelectionMemoryResult ReadNativeCheckpointSelectionWithMemoryFromOpenDevice(
    disk::FileDevice& device,const disk::NativeCommonPageHeaderBinding& expected,
    const Uuid& object,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeCheckpointSelectionMemoryError;
  NativeCheckpointSelectionMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&
        binding.database_uuid!=expected.filespace.database_uuid)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.filespace.page_size_profile_uuid);
    if(!profile||!core::uuid::IsEngineIdentityUuid(expected.filespace.filespace_uuid)||
        !core::uuid::IsEngineIdentityUuid(object)||!expected.page_number||
        !expected.page_generation||expected.page_type!=0x30e||
        (expected.page_uuid&&!core::uuid::IsEngineIdentityUuid(*expected.page_uuid))||
        expected.page_number>std::numeric_limits<u64>::max()/profile->page_size_bytes)return out;
    const auto offset=expected.page_number*profile->page_size_bytes;
    if(!disk::CheckFileDeviceExtent(offset,profile->page_size_bytes).ok())return out;
    // Node memory admission precedes the device guard. Declare the payload
    // first so every refusal unlocks the device BEFORE owning deallocation.
    auto payload=memory.AllocatePage(profile->uuid);
    out.memory_error=payload.error;out.allocation_status=payload.allocation_status;
    out.allocation_diagnostic=std::move(payload.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    const auto guard=device.AcquireOperationGuard();
    const auto bootstrap=disk::ReadFilespaceBootstrapFromOpenDevice(device,&expected.filespace);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    if(bootstrap.preamble->flags&disk::FilespaceBootstrapFlag::payload_encrypted){
      out.error=E::encrypted_requires_authority;return out;
    }
    const auto read=device.ReadAt(offset,payload.buffer.data(),payload.buffer.size());
    out.io_status=read.status;out.page_bytes_read=read.bytes_transferred;
    if(!read.ok()||read.bytes_transferred!=payload.buffer.size()){out.error=E::io_failure;return out;}
    const auto header=disk::DecodeNativeCommonPageHeader(payload.buffer.data(),128,&expected);
    if(!header.ok()){out.error=E::header_failure;out.header_error=header.error;return out;}
    auto decoded=DecodeNativeCheckpointSelectionValue({payload.buffer.data(),payload.buffer.size()});
    if(!decoded.ok()){out.error=E::selection_failure;out.selection_error=decoded.error;return out;}
    if(decoded.selection->object_uuid!=object){out.error=E::object_mismatch;return out;}
    // No separate page copy: decoded fields are fixed values, payload is the
    // actual allocation the shared manager charged before this physical read.
    out.selection=std::move(decoded.selection);out.image=std::move(payload.buffer);
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
   catch(...){out.error=E::io_failure;}
  return out;
}
} // namespace scratchbird::storage::database
