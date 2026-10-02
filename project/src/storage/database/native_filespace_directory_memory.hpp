// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_directory.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <limits>
#include <system_error>
#include <type_traits>

namespace scratchbird::storage::database {
using core::platform::usize;
using core::platform::u64;
using core::platform::Uuid;
using core::platform::byte;
enum class NativeDirectoryMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  bootstrap_failure, header_failure, directory_failure, object_mismatch,
  io_failure, resource_exhausted, lock_failure
};
// Worst-case single-image workspace, including decoded arrays and alignment.
// Every byte of this real backing is charged even when a sparse image uses less.
inline usize NativeDirectoryWorkspaceBytes(const Uuid& profile) noexcept {
  const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
  if(!p)return 0;
  const usize records=(p->page_size_bytes-384)/192;
  return p->page_size_bytes+records*(sizeof(page::NativeFilespaceDirectoryRecord)+sizeof(Uuid))+
    2*(alignof(std::max_align_t)-1);
}
struct NativeDirectoryMemoryResult {
  NativeDirectoryMemoryError error=NativeDirectoryMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  u64 page_bytes_read=0;
  NativeStorageArena arena;
  std::optional<page::NativeFilespaceDirectoryView> directory;
  std::span<const byte> image;
  bool ok() const noexcept {return error==NativeDirectoryMemoryError::none&&bool(arena)&&directory.has_value()&&!image.empty();}
};

// One inspected immutable image. No chain/current-root/reuse authority.
// All variable image/record/scratch payloads reside in the same actually admitted
// arena. The caller's node manager/ledger must outlive the returned owner.
inline NativeDirectoryMemoryResult ReadNativeFilespaceDirectoryWithMemoryFromOpenDevice(
    disk::FileDevice& device,const disk::NativeCommonPageHeaderBinding& expected,
    const Uuid& object,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeDirectoryMemoryError;
  NativeDirectoryMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=expected.filespace.database_uuid)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    const auto* profile=disk::FindCanonicalFilespacePageProfile(expected.filespace.page_size_profile_uuid);
    if(!profile||!core::uuid::IsEngineIdentityUuid(expected.filespace.filespace_uuid)||
       !core::uuid::IsEngineIdentityUuid(object)||!expected.page_number||!expected.page_generation||expected.page_type!=9||
       (expected.page_uuid&&!core::uuid::IsEngineIdentityUuid(*expected.page_uuid))||
       expected.page_number>std::numeric_limits<u64>::max()/profile->page_size_bytes)return out;
    const auto offset=expected.page_number*profile->page_size_bytes;
    if(!disk::CheckFileDeviceExtent(offset,profile->page_size_bytes).ok())return out;
    // Admit before locking and declare backing before the guard, so failure
    // unwinding unlocks the device before it calls owning memory cleanup.
    auto payload=memory.CreateArena(binding,NativeDirectoryWorkspaceBytes(profile->uuid),profile->page_size_bytes);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    auto image=payload.arena.Allocate(profile->page_size_bytes,profile->page_size_bytes);
    const usize record_count=(profile->page_size_bytes-384)/192;
    auto records=payload.arena.Allocate(record_count*sizeof(page::NativeFilespaceDirectoryRecord),alignof(page::NativeFilespaceDirectoryRecord));
    auto scratch=payload.arena.Allocate(record_count*sizeof(Uuid),alignof(Uuid));
    if(!image.ok()||!records.ok()||!scratch.ok()){out.error=E::resource_exhausted;return out;}
    static_assert(std::is_trivially_destructible_v<page::NativeFilespaceDirectoryRecord> &&
                  std::is_trivially_destructible_v<Uuid>);
    auto* record_data=static_cast<page::NativeFilespaceDirectoryRecord*>(records.pointer);
    auto* scratch_data=static_cast<Uuid*>(scratch.pointer);
    std::uninitialized_value_construct_n(record_data,record_count);
    std::uninitialized_value_construct_n(scratch_data,record_count);
    const auto guard=device.AcquireOperationGuard();
    const auto bootstrap=disk::ReadFilespaceBootstrapFromOpenDevice(device,&expected.filespace);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    // Directory metadata remains cleartext even in encrypted filespaces. This
    // structural reader supplies neither data decryption nor cluster authority.
    const auto read=device.ReadAt(offset,image.pointer,image.bytes);
    out.io_status=read.status;out.page_bytes_read=read.bytes_transferred;
    if(!read.ok()||read.bytes_transferred!=image.bytes){out.error=E::io_failure;return out;}
    const auto header=disk::DecodeNativeCommonPageHeader(static_cast<const byte*>(image.pointer),128,&expected);
    if(!header.ok()){out.error=E::header_failure;out.header_error=header.error;return out;}
    const std::span<const byte> bytes{static_cast<const byte*>(image.pointer),image.bytes};
    auto decoded=page::DecodeNativeFilespaceDirectoryInto(bytes,{record_data,record_count},{scratch_data,record_count});
    out.directory_error=decoded.error;
    if(!decoded.ok()){out.error=E::directory_failure;return out;}
    if(decoded.directory->object_uuid!=object){out.error=E::object_mismatch;return out;}
    out.directory=std::move(decoded.directory);out.image=bytes;out.arena=std::move(payload.arena);
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
