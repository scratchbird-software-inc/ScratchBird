// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include "native_storage_memory.hpp"
#include "native_metadata_memory.hpp"
#include <system_error>

namespace scratchbird::storage::database {
struct NativeInventoryChainMemoryLimits {u64 maximum_retained_image_bytes=0;std::size_t maximum_backing_bytes=0;};
enum class NativeInventoryChainMemoryError {
  none,invalid_request,memory_binding_failure,memory_allocation_failure,
  chain_failure,resource_exhausted,lock_failure
};
struct NativeInventoryChainMemoryResult {
  NativeInventoryChainMemoryError error=NativeInventoryChainMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 physical_bytes_read=0;
  NativeStorageArena arena;
  page::NativeInventoryChainView chain;
  bool ok() const noexcept{return error==NativeInventoryChainMemoryError::none&&bool(arena)&&chain.ok();}
};
// No enclosing source fence. Admit all variable image/metadata/index/guard/batch
// storage before the ordered device cohort. Every batch and the real backing
// outlives all source fences. Returned metadata outlives source close, grant
// revocation or workspace release; the issuing manager/ledger must outlive it.
// No publication-CAS base, transaction outcome, selection or cleanup authority.
inline NativeInventoryChainMemoryResult ReadNativeInventoryChainWithMemoryFromOpenDevices(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& root,NativeInventoryChainMemoryLimits limits,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding,
    page::NativeInventoryChainReadContext context=page::NativeInventoryChainReadContext::current,
    const std::array<byte,32>* root_sha256=nullptr,
    std::span<const page::NativeInventoryHistoricalPageZero> historical={}) noexcept {
  using E=NativeInventoryChainMemoryError;
  NativeInventoryChainMemoryResult out;
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
    disk::detail::NativeMetadataMemory resource(region);disk::detail::NativeMetadataScratch scratch{&resource};
    using Batch=disk::FileDevice::ReadLatencyBatch;
    auto pointers=scratch.Array<Batch*>(files.size());
    if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(Batch))throw std::bad_alloc();
    auto* batches=static_cast<Batch*>(resource.allocate(files.size()*sizeof(Batch),alignof(Batch)));
    {
      struct Cleanup {Batch* batches;std::size_t count=0;
        ~Cleanup(){while(count)std::destroy_at(batches+--count);}
      } cleanup{batches};
      for(std::size_t i=0;i<files.size();++i){pointers[i]=std::construct_at(batches+i,*files[i].device);++cleanup.count;}
      const auto prefix=resource.used();
      auto read=page::ReadNativeTransactionInventoryChainInto(database,files,root,
        limits.maximum_retained_image_bytes,context,root_sha256,historical,pointers,region.subspan(prefix));
      out.inventory_error=read.chain.error;out.io_status=read.io_status;
      out.io_diagnostic=std::move(read.io_diagnostic);out.physical_bytes_read=read.physical_bytes_read;
      if(!read.ok()){out.error=E::chain_failure;return out;}
      out.chain=read.chain;out.chain.backing_bytes_used+=prefix;
    }
    out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
