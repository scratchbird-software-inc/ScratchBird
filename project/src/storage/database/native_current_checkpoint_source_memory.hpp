// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_dirty_manifest.hpp"
#include "native_storage_memory.hpp"
#include "native_metadata_decode_scratch.hpp"
#include <system_error>
#include <type_traits>
namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
struct NativeCurrentCheckpointMemoryLimits {
 u64 maximum_verification_image_bytes=0;
 std::size_t maximum_backing_bytes=0;
};
enum class NativeCurrentCheckpointMemoryError {
 none,invalid_request,memory_binding_failure,memory_allocation_failure,
 source_failure,resource_exhausted,lock_failure
};
template<class View> struct NativeCurrentCheckpointMemoryResult {
 NativeCurrentCheckpointMemoryError error=NativeCurrentCheckpointMemoryError::invalid_request;
 NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
 NativeCheckpointError source_error=NativeCheckpointError::none;
 page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
 page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
 page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
 core::platform::Status allocation_status,io_status;
 core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
 u64 physical_bytes_read=0;
 NativeStorageArena arena;
 View source;
 bool ok() const noexcept{return error==NativeCurrentCheckpointMemoryError::none&&bool(arena)&&source.ok();}
};
using NativeCurrentCheckpointAllocationMemoryResult=NativeCurrentCheckpointMemoryResult<NativeCheckpointAllocationView>;
using NativeCurrentCheckpointDirectoryMemoryResult=NativeCurrentCheckpointMemoryResult<NativeCheckpointDirectoryView>;
namespace detail {
template<class View,class Reader> NativeCurrentCheckpointMemoryResult<View> ReadCurrentCheckpointWithMemory(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 NativeCurrentCheckpointMemoryLimits limits,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,
 Reader reader) noexcept {
 using E=NativeCurrentCheckpointMemoryError;NativeCurrentCheckpointMemoryResult<View> out;
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
  NativeMetadataMemory resource(region);NativeMetadataScratch scratch{&resource};
  using Batch=disk::FileDevice::ReadLatencyBatch;auto pointers=scratch.Array<Batch*>(files.size());
  if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(Batch))throw std::bad_alloc();
  auto* batches=static_cast<Batch*>(resource.allocate(files.size()*sizeof(Batch),alignof(Batch)));
  {
   struct Cleanup{Batch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
   for(std::size_t n=0;n<files.size();++n){pointers[n]=std::construct_at(batches+n,*files[n].device);++cleanup.count;}
   const auto prefix=resource.used();
   auto read=reader(database,files,root,limits.maximum_verification_image_bytes,pointers,region.subspan(prefix));
   out.io_status=read.io_status;out.io_diagnostic=std::move(read.io_diagnostic);out.physical_bytes_read=read.physical_bytes_read;
   const auto& source=[&]() -> const View& {
    if constexpr(std::is_same_v<View,NativeCheckpointAllocationView>)return read.allocation;
    else return read.directory;
   }();
   out.source_error=source.error;out.inventory_error=source.checkpoint_inventory.inventory_error;
   if constexpr(std::is_same_v<View,NativeCheckpointAllocationView>)out.allocation_error=source.allocation_error;
   else out.directory_error=source.directory_error;
   if(!read.ok()){out.error=E::source_failure;return out;}
   out.source=source;out.source.backing_bytes_used+=prefix;
  }
  out.arena=std::move(payload.arena);out.error=E::none;return out;
 }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
  catch(const std::length_error&){out.error=E::resource_exhausted;}
  catch(const std::system_error&){out.error=E::lock_failure;}
 return out;
}
}
// No enclosing source fences: admit actual backing/batches before the complete
// ordered cohort, and retire observations/payload only after its release. The
// issuer must outlive every retained result; revocation cannot release live data.
// Full inspection does not confer policy/security, allocation or serving rights.
inline NativeCurrentCheckpointAllocationMemoryResult VerifyCurrentNativeCheckpointAllocationWithMemoryFromOpenDevices(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 NativeCurrentCheckpointMemoryLimits limits,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
 return detail::ReadCurrentCheckpointWithMemory<NativeCheckpointAllocationView>(database,files,root,limits,memory,binding,VerifyCurrentNativeCheckpointAllocationInto);
}
inline NativeCurrentCheckpointDirectoryMemoryResult VerifyCurrentNativeCheckpointDirectoryWithMemoryFromOpenDevices(
 const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const disk::FilespaceRootReference& root,
 NativeCurrentCheckpointMemoryLimits limits,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
 return detail::ReadCurrentCheckpointWithMemory<NativeCheckpointDirectoryView>(database,files,root,limits,memory,binding,VerifyCurrentNativeCheckpointDirectoryInto);
}
} // namespace scratchbird::storage::database
