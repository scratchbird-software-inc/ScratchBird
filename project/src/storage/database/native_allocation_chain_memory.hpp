// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_allocation_map.hpp"
#include "native_storage_memory.hpp"
#include <system_error>

namespace scratchbird::storage::database {
struct NativeAllocationChainMemoryLimits {
  u64 maximum_retained_image_bytes=0;
  std::size_t maximum_backing_bytes=0;
};
enum class NativeAllocationChainMemoryError {
  none,invalid_request,memory_binding_failure,memory_allocation_failure,
  chain_failure,resource_exhausted,lock_failure
};
struct NativeAllocationChainMemoryResult {
  NativeAllocationChainMemoryError error=NativeAllocationChainMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  page::NativeAllocationError chain_error=page::NativeAllocationError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  NativeStorageArena arena;
  page::NativeAllocationChainView chain;
  bool ok() const noexcept{return error==NativeAllocationChainMemoryError::none&&bool(arena)&&chain.ok();}
};
// No enclosing device fence. All variable chain images/metadata/indexes use
// this actual binary-bound grant before taking the source fence. Observations
// and backing cleanup run after it releases. The manager/ledger must outlive the
// returned owner. No implied checkpoint, visibility, free-page or execution grant.
inline NativeAllocationChainMemoryResult ReadNativeAllocationChainWithMemoryFromOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& source,
    NativeAllocationChainMemoryLimits limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding,
    page::NativeAllocationChainReadContext context=page::NativeAllocationChainReadContext::bootstrap,
    const disk::FilespaceRootReference* root=nullptr,
    const std::array<byte,32>* root_sha256=nullptr,std::span<const byte> historical={}) noexcept {
  using E=NativeAllocationChainMemoryError;
  NativeAllocationChainMemoryResult out;
  try{
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=source.database_uuid)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    if(!limits.maximum_backing_bytes)return out;
    auto payload=memory.CreateArena(binding,limits.maximum_backing_bytes,alignof(std::max_align_t));
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    const auto allocation=payload.arena.Allocate(limits.maximum_backing_bytes,alignof(std::max_align_t));
    if(!allocation.ok()){out.error=E::resource_exhausted;return out;}
    {
      disk::FileDevice::ReadLatencyBatch observations(device);
      auto read=page::ReadNativeAllocationChainInto(device,source,limits.maximum_retained_image_bytes,
        context,root,root_sha256,historical,observations,
        {static_cast<byte*>(allocation.pointer),allocation.bytes});
      out.chain_error=read.chain.error;out.io_status=read.io_status;
      out.io_diagnostic=std::move(read.io_diagnostic);out.physical_bytes_read=read.physical_bytes_read;
      if(!read.ok()){out.error=E::chain_failure;return out;}
      out.chain=read.chain;
    }
    out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
