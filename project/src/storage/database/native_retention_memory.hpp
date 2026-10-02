// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_retention_root.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <limits>
#include <system_error>
#include <type_traits>

namespace scratchbird::storage::database {
using core::platform::usize;
using core::platform::u64;
using core::platform::u32;
using core::platform::Uuid;
using core::platform::byte;
enum class NativeRetentionMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  bootstrap_failure, header_failure, retention_failure, object_mismatch,
  io_failure, resource_exhausted, lock_failure, encrypted_requires_authority
};
// Worst-case single-image workspace, including leaf records and alignment.
// Every byte of this real backing is charged even when a sparse image uses less.
inline usize NativeRetentionWorkspaceBytes(const Uuid& profile,u32 type) noexcept {
  const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
  if(!p||(type!=0x303&&type!=0x304))return 0;
  if(type==0x303)return p->page_size_bytes;
  const auto count=(p->page_size_bytes-384)/160;
  return p->page_size_bytes+count*sizeof(page::NativeRetentionPin)+(alignof(std::max_align_t)-1);
}
struct NativeRetentionMemoryResult {
  NativeRetentionMemoryError error=NativeRetentionMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
  page::NativeRetentionError retention_error=page::NativeRetentionError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  u64 page_bytes_read=0;
  NativeStorageArena arena;
  std::optional<page::NativeRetentionPageView> page;
  std::span<const byte> image;
  bool ok() const noexcept {return error==NativeRetentionMemoryError::none&&bool(arena)&&page.has_value()&&!image.empty();}
};

// One inspected immutable image. No chain/current-root/reuse authority.
// All variable image/record payloads reside in the same actually admitted
// arena. The caller's node manager/ledger must outlive the returned owner.
inline NativeRetentionMemoryResult ReadNativeRetentionWithMemoryFromOpenDevice(
    disk::FileDevice& device,const disk::NativeCommonPageHeaderBinding& expected,
    const Uuid& object,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeRetentionMemoryError;
  NativeRetentionMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=expected.filespace.database_uuid)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.filespace.page_size_profile_uuid);
    if(!profile||!core::uuid::IsEngineIdentityUuid(expected.filespace.filespace_uuid)||
       !core::uuid::IsEngineIdentityUuid(object)||!expected.page_number||!expected.page_generation||(expected.page_type!=0x303&&expected.page_type!=0x304)||
       (expected.page_uuid&&!core::uuid::IsEngineIdentityUuid(*expected.page_uuid))||
       expected.page_number>std::numeric_limits<u64>::max()/profile->page_size_bytes)return out;
    const auto offset=expected.page_number*profile->page_size_bytes;
    if(!disk::CheckFileDeviceExtent(offset,profile->page_size_bytes).ok())return out;
    // Admit before locking and declare backing before the guard, so failure
    // unwinding unlocks the device before it calls owning memory cleanup.
    auto payload=memory.CreateArena(binding,NativeRetentionWorkspaceBytes(profile->uuid,expected.page_type),profile->page_size_bytes);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    auto image=payload.arena.Allocate(profile->page_size_bytes,profile->page_size_bytes);
    const usize count=expected.page_type==0x303?0:(profile->page_size_bytes-384)/160;
    using Record=page::NativeRetentionPin;
    std::span<Record> records;
    if(!image.ok()){out.error=E::resource_exhausted;return out;}
    if(count){
      auto native=payload.arena.Allocate(count*sizeof(Record),alignof(Record));
      if(!native.ok()){out.error=E::resource_exhausted;return out;}
      static_assert(std::is_trivially_destructible_v<Record>);
      auto* data=static_cast<Record*>(native.pointer);
      std::uninitialized_value_construct_n(data,count);records={data,count};
    }
    const auto guard=device.AcquireOperationGuard();
    const auto bootstrap=disk::ReadFilespaceBootstrapFromOpenDevice(device,&expected.filespace);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    if(bootstrap.preamble->flags&disk::FilespaceBootstrapFlag::payload_encrypted){
      out.error=E::encrypted_requires_authority;return out;
    }
    auto read=device.ReadAt(offset,image.pointer,image.bytes);
    out.io_status=read.status;out.io_diagnostic=std::move(read.diagnostic);out.page_bytes_read=read.bytes_transferred;
    if(!read.ok()||read.bytes_transferred!=image.bytes){out.error=E::io_failure;return out;}
    const auto header=disk::DecodeNativeCommonPageHeader(static_cast<const byte*>(image.pointer),128,&expected);
    if(!header.ok()){out.error=E::header_failure;out.header_error=header.error;return out;}
    if(header.header->flags&1u){out.error=E::encrypted_requires_authority;return out;}
    const std::span<const byte> bytes{static_cast<const byte*>(image.pointer),image.bytes};
    auto decoded=page::DecodeNativeRetentionPageInto(bytes,records);
    out.retention_error=decoded.error;
    if(!decoded.ok()){out.error=E::retention_failure;return out;}
    if(decoded.page->object_uuid!=object){out.error=E::object_mismatch;return out;}
    out.page=std::move(decoded.page);out.image=bytes;out.arena=std::move(payload.arena);
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
