// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_authority.hpp"
#include "native_storage_memory.hpp"
#include "native_metadata_decode_scratch.hpp"
#include <system_error>

namespace scratchbird::storage::database {
struct NativeManagementControlAuthorityMemoryLimits {
  u64 maximum_verification_image_bytes=0;
  std::size_t maximum_backing_bytes=0;
};
enum class NativeManagementControlAuthorityMemoryError {
  none,invalid_request,memory_binding_failure,memory_allocation_failure,
  authority_failure,resource_exhausted,lock_failure
};
struct NativeManagementControlAuthorityMemoryResult {
  NativeManagementControlAuthorityMemoryError error=NativeManagementControlAuthorityMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  NativeManagementControlAuthorityError authority_error=NativeManagementControlAuthorityError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  NativeStorageArena arena;
  NativeManagementControlAuthorityView authority;
  bool ok() const noexcept{return error==NativeManagementControlAuthorityMemoryError::none&&bool(arena)&&authority.ok();}
};
// No enclosing device fence is permitted. The retained real binary-bound grant
// backs all images, decoded metadata, indexes, lock storage and read-observation
// batches before the ordered device cohort is locked. Observations and cleanup
// run only after all those fences release. No implicit open/close, write,
// publication, execution or recovery authority; revocation cannot erase live
// retained bytes/accounting. The issuing manager must outlive the result.
inline NativeManagementControlAuthorityMemoryResult ReadNativeManagementControlAuthorityWithMemoryFromOpenDevices(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,
    NativeManagementControlAuthorityMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding,
    NativeManagementHistoryReadContext context=NativeManagementHistoryReadContext::selected,
    const NativeManagementCheckpointAnchor* anchor=nullptr,
    std::span<const NativeManagementHistoricalPageZero> historical={}) noexcept {
  using E=NativeManagementControlAuthorityMemoryError;
  NativeManagementControlAuthorityMemoryResult out;
  try{
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(files.empty()||!limits.maximum_backing_bytes)return out;
    for(const auto& file:files)if(!file.device)return out;
    auto payload=memory.CreateArena(binding,limits.maximum_backing_bytes,alignof(std::max_align_t));
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    const auto allocation=payload.arena.Allocate(limits.maximum_backing_bytes,alignof(std::max_align_t));
    if(!allocation.ok()){out.error=E::resource_exhausted;return out;}
    const std::span<byte> region{static_cast<byte*>(allocation.pointer),allocation.bytes};
    detail::NativeMetadataMemory resource(region);detail::NativeMetadataScratch scratch{&resource};
    using Batch=disk::FileDevice::ReadLatencyBatch;
    auto pointers=scratch.Array<Batch*>(files.size());
    if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(Batch))throw std::bad_alloc();
    auto* batches=static_cast<Batch*>(resource.allocate(files.size()*sizeof(Batch),alignof(Batch)));
    {
      struct Cleanup {
        Batch* batches;std::size_t count=0;
        ~Cleanup(){while(count)std::destroy_at(batches+--count);}
      } cleanup{batches};
      for(std::size_t i=0;i<files.size();++i){
        pointers[i]=std::construct_at(batches+i,*files[i].device);++cleanup.count;
      }
      const auto prefix=resource.used();
      auto result=ReadNativeManagementControlAuthorityInto(database,files,primary,
        limits.maximum_verification_image_bytes,context,anchor,historical,pointers,region.subspan(prefix));
      out.authority_error=result.authority.error;out.io_status=result.io_status;
      out.io_diagnostic=std::move(result.io_diagnostic);out.physical_bytes_read=result.physical_bytes_read;
      if(!result.ok()){out.error=E::authority_failure;return out;}
      out.authority=result.authority;out.authority.backing_bytes_used+=prefix;
    }
    out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
