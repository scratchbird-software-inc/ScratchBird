// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "physical_mga_cow_store.hpp"
#include "native_storage_memory.hpp"
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include <algorithm>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>

namespace scratchbird::storage::database {
inline core::platform::usize NativeCatalogLeafWorkspaceBytes(const core::platform::Uuid& profile) noexcept {
  const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
  if(!p)return 0;
  const auto n=page::RowDataPageViewBackingRequirements(p->page_size_bytes-160);
  return p->page_size_bytes+32*sizeof(disk::FilespaceRootReference)+
    n.rows*(sizeof(page::RowDataRecordView)+sizeof(page::RowDataSlot)+sizeof(NativeCatalogLeafRecordView))+
    n.cells*sizeof(page::RowDataCellView)+2*n.index_slots*sizeof(core::platform::u32)+
    7*(alignof(std::max_align_t)-1);
}
enum class NativeCatalogLeafMemoryError {
  none, invalid_request, memory_binding_failure, memory_allocation_failure,
  bootstrap_failure, page_zero_failure, binding_mismatch, leaf_failure,
  io_failure, resource_exhausted, lock_failure, encrypted_requires_authority,
  cluster_requires_authority, header_policy_requires_authority
};
struct NativeCatalogLeafMemoryResult {
  NativeCatalogLeafMemoryError error=NativeCatalogLeafMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
  core::platform::Status allocation_status,io_status;
  core::platform::DiagnosticRecord allocation_diagnostic,io_diagnostic;
  core::platform::u64 bootstrap_bytes_read=0,page_zero_bytes_read=0,page_bytes_read=0;
  NativeStorageArena arena;
  NativeCatalogLeafViewResult leaf;
  std::span<const core::platform::byte> image;
  bool ok() const noexcept {return error==NativeCatalogLeafMemoryError::none&&bool(arena)&&leaf.ok()&&!image.empty();}
};
// Real single-image observation, not current-root/visibility/publication authority.
// All image/row/cell/slot/metadata/name backing consumes this retained actual grant.
// Admit before device fences; the node manager and ledger outlive returned owners.
// The caller must not already hold a device fence. Optional metric publication
// runs synchronously after this reader releases its own fence, not under it.
inline NativeCatalogLeafMemoryResult ReadNativeCatalogLeafWithMemoryFromOpenDevice(
    disk::FileDevice& device,const core::platform::Uuid& database,
    const page::NativeCatalogRootReference& ref,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeCatalogLeafMemoryError;
  using core::platform::byte;
  NativeCatalogLeafMemoryResult out;
  try {
    out.memory_error=memory.CheckBinding(binding);
    if(out.memory_error==NativeStorageMemoryError::none&&binding.database_uuid!=database)
      out.memory_error=NativeStorageMemoryError::invalid_binding;
    if(out.memory_error!=NativeStorageMemoryError::none){out.error=E::memory_binding_failure;return out;}
    const auto& target=ref.page;
    const auto* profile=disk::FindCanonicalFilespacePageProfile(target.page_size_profile_uuid);
    if(!profile||ref.role<1||ref.role>6||ref.page_type!=6||!core::uuid::IsEngineIdentityUuid(database)||
        !core::uuid::IsEngineIdentityUuid(ref.object_uuid)||!core::uuid::IsEngineIdentityUuid(target.filespace_uuid)||
        !target.page_number||!target.page_generation||
        target.page_number>std::numeric_limits<core::platform::u64>::max()/profile->page_size_bytes)return out;
    const auto offset=target.page_number*profile->page_size_bytes;
    if(!disk::CheckFileDeviceExtent(offset,profile->page_size_bytes).ok())return out;
    auto payload=memory.CreateArena(binding,NativeCatalogLeafWorkspaceBytes(profile->uuid),profile->page_size_bytes);
    out.memory_error=payload.error;out.allocation_status=payload.backing.status;
    out.allocation_diagnostic=std::move(payload.backing.diagnostic);
    if(!payload.ok()){out.error=E::memory_allocation_failure;return out;}
    const auto image=payload.arena.Allocate(profile->page_size_bytes,profile->page_size_bytes);
    const auto n=page::RowDataPageViewBackingRequirements(profile->page_size_bytes-160);
    bool allocated=image.ok();
    const auto array=[&]<class T>(std::size_t count)->std::span<T>{
      static_assert(std::is_trivially_destructible_v<T>);
      if(!count)return {};
      const auto block=payload.arena.Allocate(count*sizeof(T),alignof(T));
      if(!block.ok()){allocated=false;return {};}
      auto* data=static_cast<T*>(block.pointer);std::uninitialized_value_construct_n(data,count);return {data,count};
    };
    auto roots=array.template operator()<disk::FilespaceRootReference>(32);
    page::RowDataPageViewWorkspace workspace;
    workspace.rows=array.template operator()<page::RowDataRecordView>(n.rows);
    workspace.cells=array.template operator()<page::RowDataCellView>(n.cells);
    workspace.slots=array.template operator()<page::RowDataSlot>(n.rows);
    workspace.version_index=array.template operator()<core::platform::u32>(n.index_slots);
    workspace.sequence_index=array.template operator()<core::platform::u32>(n.index_slots);
    auto metadata=array.template operator()<NativeCatalogLeafRecordView>(n.rows);
    if(!allocated){out.error=E::resource_exhausted;return out;}
    // All governor/physical backing calls precede this fence. Destruction of
    // the fence precedes arena cleanup on every refusal or exception path.
    disk::FileDevice::ReadLatencyBatch observations(device);
    const auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding expected{database,target.filespace_uuid,profile->uuid};
    disk::SerializedFilespaceBootstrap prefix{};
    auto probe=observations.ReadAt(0,prefix.data(),prefix.size());
    out.bootstrap_bytes_read=probe.bytes_transferred;out.io_status=probe.status;out.io_diagnostic=std::move(probe.diagnostic);
    if(!probe.ok()||probe.bytes_transferred!=prefix.size()){out.error=E::io_failure;return out;}
    const auto bootstrap=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&expected);
    if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
    auto before=device.Size();out.io_status=before.status;out.io_diagnostic=std::move(before.diagnostic);
    if(!before.ok()){out.error=E::io_failure;return out;}
    auto read=observations.ReadAt(0,image.pointer,image.bytes);
    out.page_zero_bytes_read=read.bytes_transferred;out.io_status=read.status;out.io_diagnostic=std::move(read.diagnostic);
    if(!read.ok()||read.bytes_transferred!=image.bytes){out.error=E::io_failure;return out;}
    const std::span<const byte> bytes{static_cast<const byte*>(image.pointer),image.bytes};
    if(!std::equal(prefix.begin(),prefix.end(),bytes.begin())){
      out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::probe_changed;return out;
    }
    const auto zero=disk::DecodeFilespacePageZeroInto(bytes,roots,&expected);
    if(!zero.ok()){out.error=E::page_zero_failure;out.page_zero_error=zero.error;return out;}
    if(before.size_bytes!=zero.record->total_pages*profile->page_size_bytes||
        target.page_number>=zero.record->total_pages||zero.record->bootstrap.filespace_role>5){
      out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;
    }
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted){out.error=E::encrypted_requires_authority;return out;}
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required){out.error=E::cluster_requires_authority;return out;}
    read=observations.ReadAt(offset,image.pointer,image.bytes);
    out.page_bytes_read=read.bytes_transferred;out.io_status=read.status;out.io_diagnostic=std::move(read.diagnostic);
    if(!read.ok()||read.bytes_transferred!=image.bytes){out.error=E::io_failure;return out;}
    auto after=device.Size();out.io_status=after.status;out.io_diagnostic=std::move(after.diagnostic);
    if(!after.ok()){out.error=E::io_failure;return out;}
    if(before.size_bytes!=after.size_bytes){out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;}
    const disk::NativeCommonPageHeaderBinding page_binding{expected,target.page_number,target.page_generation,6,{}};
    const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&page_binding);
    if(!header.ok()){out.error=E::binding_mismatch;out.header_error=header.error;return out;}
    if(header.header->flags&1u){out.error=E::encrypted_requires_authority;return out;}
    if(header.header->flags&2u){out.error=E::cluster_requires_authority;return out;}
    if(header.header->flags&12u){out.error=E::header_policy_requires_authority;return out;}
    auto leaf=DecodeNativeCatalogLeafInto(bytes,workspace,metadata);
    if(!leaf.ok()){out.error=E::leaf_failure;out.leaf=std::move(leaf);return out;}
    if(leaf.page->body.relation_uuid.value!=ref.object_uuid){out.error=E::binding_mismatch;return out;}
    out.leaf=std::move(leaf);out.image=bytes;out.arena=std::move(payload.arena);out.error=E::none;return out;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
} // namespace scratchbird::storage::database
