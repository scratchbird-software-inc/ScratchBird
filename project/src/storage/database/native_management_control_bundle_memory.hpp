// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_bundle.hpp"
#include "native_storage_memory.hpp"
#include <limits>
#include <system_error>

namespace scratchbird::storage::database {
struct NativeManagementControlBundleMemoryLimits {
  u64 maximum_verification_image_bytes=0;
  std::size_t maximum_backing_bytes=0;
};
enum class NativeManagementControlBundleMemoryError {
  none,invalid_request,memory_binding_failure,memory_allocation_failure,
  bundle_failure,resource_exhausted,lock_failure
};
struct NativeManagementControlBundleMemoryResult {
  NativeManagementControlBundleMemoryError error=NativeManagementControlBundleMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  NativeManagementControlBundleError bundle_error=NativeManagementControlBundleError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  NativeStorageArena arena;
  NativeManagementControlBundleViewRead bundle;
  bool ok() const noexcept{return error==NativeManagementControlBundleMemoryError::none&&bool(arena)&&bundle.ok();}
};
// No outer device fence is permitted here. The actual shared grant backs the
// entire retained image/metadata workspace before any read guard is acquired.
// Observations publish after that guard releases; cleanup likewise occurs only
// after the read fence. Retained bytes survive source close and memory revocation
// until their last owning result is released. Context images are not authority.
inline NativeManagementControlBundleMemoryResult ReadNativeManagementControlBundleWithMemoryFromOpenDevice(
    const disk::NativeFilespaceDevice& file,const NativeManagementControlBundleRoot& root,
    const Uuid& database,const Uuid& bootstrap,NativeManagementControlBundleMemoryLimits limits,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,
    NativeManagementControlReadContext context=NativeManagementControlReadContext::current,
    std::span<const byte> historical={}) noexcept {
  using E=NativeManagementControlBundleMemoryError;
  NativeManagementControlBundleMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(!file.device||!limits.maximum_backing_bytes)return out;
    out.bundle_error=ValidateNativeManagementControlBundleRoot(root,database,bootstrap,limits.maximum_verification_image_bytes);
    if(out.bundle_error!=NativeManagementControlBundleError::none){out.error=E::bundle_failure;return out;}
    if(file.filespace_uuid!=root.first.filespace_uuid||file.page_size_profile_uuid!=root.first.page_size_profile_uuid||
       (context!=NativeManagementControlReadContext::current&&
        context!=NativeManagementControlReadContext::historical_before&&
        context!=NativeManagementControlReadContext::historical_result)||
       (context==NativeManagementControlReadContext::current&&!historical.empty()))return out;
    const auto* profile=disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid);
    auto payload=memory.CreateArena(binding,limits.maximum_backing_bytes,profile->page_size_bytes);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    const auto region=payload.arena.Allocate(limits.maximum_backing_bytes,profile->page_size_bytes);
    if(!region.ok()){out.error=E::resource_exhausted;return out;}
    // This scope must end before payload cleanup or returning its retained owner.
    {
      disk::FileDevice::ReadLatencyBatch observations(*file.device);
      auto view=ReadNativeManagementControlBundleInto(file,root,database,bootstrap,
        limits.maximum_verification_image_bytes,context,historical,observations,
        {static_cast<byte*>(region.pointer),region.bytes});
      out.bundle_error=view.bundle.error;out.io_status=view.io_status;
      out.io_diagnostic=std::move(view.io_diagnostic);out.physical_bytes_read=view.physical_bytes_read;
      if(!view.ok()){out.error=E::bundle_failure;return out;}
      out.bundle=view.bundle;
    }
    out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
