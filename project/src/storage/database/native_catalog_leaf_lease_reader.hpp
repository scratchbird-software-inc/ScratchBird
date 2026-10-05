// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_leaf_memory.hpp"
#include "native_selected_checkpoint_memory_lease.hpp"

namespace scratchbird::storage::database {
class NativeCatalogLeafLeaseReader;
struct NativeCatalogLeafPreparedResult;
enum class NativeCatalogLeafProfileMode { exact, maximum };

// Admit before acquiring the source lease; destroy after releasing it. Reusing
// this workspace invalidates earlier views. It never owns or releases a device
// fence. The governor/ledger must outlive its actual retained arena.
class NativeCatalogLeafPreparedMemory {
 public:
  NativeCatalogLeafPreparedMemory() noexcept=default;
  NativeCatalogLeafPreparedMemory(const NativeCatalogLeafPreparedMemory&)=delete;
  NativeCatalogLeafPreparedMemory& operator=(const NativeCatalogLeafPreparedMemory&)=delete;
  NativeCatalogLeafPreparedMemory(NativeCatalogLeafPreparedMemory&&) noexcept=default;
  NativeCatalogLeafPreparedMemory& operator=(NativeCatalogLeafPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept{return bool(arena_);}
 private:
  NativeStorageArena arena_;
  Uuid profile_;
  NativeCatalogLeafProfileMode profile_mode_=NativeCatalogLeafProfileMode::exact;
  std::span<byte> image_;
  std::span<disk::FilespaceRootReference> roots_;
  page::RowDataPageViewWorkspace rows_;
  std::span<NativeCatalogLeafRecordView> metadata_;
  friend class NativeCatalogLeafLeaseReader;
  friend NativeCatalogLeafPreparedResult PrepareNativeCatalogLeafRead(
      const Uuid&,NativeStorageMemory&,const NativeStorageMemoryBinding&,NativeCatalogLeafProfileMode) noexcept;
};
struct NativeCatalogLeafPreparedResult {
  NativeCatalogLeafMemoryError error=NativeCatalogLeafMemoryError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeCatalogLeafPreparedMemory workspace;
  bool ok() const noexcept{return error==NativeCatalogLeafMemoryError::none&&bool(workspace);}
};
inline NativeCatalogLeafPreparedResult PrepareNativeCatalogLeafRead(
    const Uuid& profile,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding,
    NativeCatalogLeafProfileMode mode=NativeCatalogLeafProfileMode::exact) noexcept {
  using E=NativeCatalogLeafMemoryError;
  NativeCatalogLeafPreparedResult out;
  try {
    const auto* p=disk::FindCanonicalFilespacePageProfile(profile);
    if(!p||(mode!=NativeCatalogLeafProfileMode::exact&&mode!=NativeCatalogLeafProfileMode::maximum))return out;
    auto granted=memory.CreateArena(binding,NativeCatalogLeafWorkspaceBytes(profile),p->page_size_bytes);
    out.memory_error=granted.error;out.allocation_status=granted.backing.status;
    out.allocation_diagnostic=std::move(granted.backing.diagnostic);
    if(!granted.ok()){out.error=E::memory_allocation_failure;return out;}
    NativeCatalogLeafPreparedMemory prepared;
    prepared.arena_=std::move(granted.arena);prepared.profile_=profile;prepared.profile_mode_=mode;
    const auto array=[&]<class T>(std::size_t count)->std::span<T>{
      static_assert(std::is_trivially_destructible_v<T>);
      if(!count)return {};
      const auto block=prepared.arena_.Allocate(count*sizeof(T),alignof(T));
      if(!block.ok())throw std::bad_alloc();
      auto* data=static_cast<T*>(block.pointer);
      std::uninitialized_value_construct_n(data,count);return {data,count};
    };
    const auto image=prepared.arena_.Allocate(p->page_size_bytes,p->page_size_bytes);
    if(!image.ok()){out.error=E::resource_exhausted;return out;}
    prepared.image_={static_cast<byte*>(image.pointer),image.bytes};
    prepared.roots_=array.template operator()<disk::FilespaceRootReference>(32);
    const auto n=page::RowDataPageViewBackingRequirements(p->page_size_bytes-160);
    prepared.rows_.rows=array.template operator()<page::RowDataRecordView>(n.rows);
    prepared.rows_.cells=array.template operator()<page::RowDataCellView>(n.cells);
    prepared.rows_.slots=array.template operator()<page::RowDataSlot>(n.rows);
    prepared.rows_.version_index=array.template operator()<u32>(n.index_slots);
    prepared.rows_.sequence_index=array.template operator()<u32>(n.index_slots);
    prepared.metadata_=array.template operator()<NativeCatalogLeafRecordView>(n.rows);
    out.workspace=std::move(prepared);out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
  return out;
}

struct NativeCatalogLeafLeaseReadResult {
  NativeCatalogLeafMemoryError error=NativeCatalogLeafMemoryError::invalid_request;
  disk::FilespaceBootstrapError bootstrap_error=disk::FilespaceBootstrapError::none;
  disk::FilespacePageZeroError page_zero_error=disk::FilespacePageZeroError::none;
  disk::NativeCommonPageHeaderError header_error=disk::NativeCommonPageHeaderError::none;
  core::platform::Status io_status;
  core::platform::DiagnosticRecord io_diagnostic;
  u64 bytes_read=0;
  NativeCatalogLeafViewResult leaf;
  std::span<const byte> image;
  bool ok() const noexcept{return error==NativeCatalogLeafMemoryError::none&&leaf.ok()&&!image.empty();}
};

// Leaf observation beneath the existing privately admitted source lease. This
// does NOT establish selected catalog membership, index coverage, creator/MGA
// visibility, security, allocation or publication. The graph reader must bind
// those separately. In particular, a caller-supplied ref is not catalog authority.
class NativeCatalogLeafLeaseReader {
 public:
  static NativeCatalogLeafLeaseReadResult Read(
      const NativeSelectedCheckpointMemoryLease& lease,
      const page::NativeCatalogRootReference& ref,
      NativeCatalogLeafPreparedMemory& backing) noexcept {
    using E=NativeCatalogLeafMemoryError;
    NativeCatalogLeafLeaseReadResult out;
    try {
      lease.CheckThread();
      if(!backing||!lease.selection_.checkpoint_inventory.checkpoint)return out;
      const auto database=lease.selection_.checkpoint_inventory.checkpoint->header.database_uuid;
      const auto& target=ref.page;
      const auto* profile=disk::FindCanonicalFilespacePageProfile(target.page_size_profile_uuid);
      if(!profile||backing.arena_.binding().database_uuid!=database||
          (backing.profile_mode_==NativeCatalogLeafProfileMode::exact&&backing.profile_!=target.page_size_profile_uuid)||
          ref.role<1||ref.role>6||ref.page_type!=6||!core::uuid::IsEngineIdentityUuid(ref.object_uuid)||
          !target.page_number||!target.page_generation)return out;
      if(profile->page_size_bytes>backing.image_.size()){out.error=E::resource_exhausted;return out;}
      const auto image=backing.image_.first(profile->page_size_bytes);
      const auto found=std::find_if(lease.devices_.begin(),lease.devices_.end(),
          [&](const auto& f){return f.filespace_uuid==target.filespace_uuid&&f.page_size_profile_uuid==target.page_size_profile_uuid;});
      if(found==lease.devices_.end()){out.error=E::binding_mismatch;return out;}
      const auto ordinal=static_cast<std::size_t>(found-lease.devices_.begin());
      auto& device=*found->device;auto& observations=lease.batches_[ordinal];
      const auto size=image.size();
      if(target.page_number>std::numeric_limits<u64>::max()/size)return out;
      const auto offset=target.page_number*size;
      if(!disk::CheckFileDeviceExtent(offset,size).ok())return out;
      // No grant checks, new batches, owned scratch or metric callbacks here.
      // The existing lease's batches publish only after all its guards release.
      const auto read=[&](u64 at,void* data,std::size_t bytes){
        auto io=observations.ReadAt(at,data,bytes);out.bytes_read+=io.bytes_transferred;
        out.io_status=io.status;out.io_diagnostic=std::move(io.diagnostic);
        return io.ok()&&io.bytes_transferred==bytes;
      };
      const disk::FilespaceBootstrapBinding expected{database,target.filespace_uuid,target.page_size_profile_uuid};
      disk::SerializedFilespaceBootstrap prefix{};
      if(!read(0,prefix.data(),prefix.size())){out.error=E::io_failure;return out;}
      const auto bootstrap=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&expected);
      if(!bootstrap.ok()){out.error=E::bootstrap_failure;out.bootstrap_error=bootstrap.error;return out;}
      auto before=device.Size();out.io_status=before.status;out.io_diagnostic=std::move(before.diagnostic);
      if(!before.ok()){out.error=E::io_failure;return out;}
      if(!read(0,image.data(),size)){out.error=E::io_failure;return out;}
      if(!std::equal(prefix.begin(),prefix.end(),image.begin())){
        out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::probe_changed;return out;
      }
      const auto zero=disk::DecodeFilespacePageZeroInto(image,backing.roots_,&expected);
      if(!zero.ok()){out.error=E::page_zero_failure;out.page_zero_error=zero.error;return out;}
      if(zero.record->total_pages>std::numeric_limits<u64>::max()/size||
          before.size_bytes!=zero.record->total_pages*size||target.page_number>=zero.record->total_pages||
          zero.record->bootstrap.filespace_role>5){
        out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;
      }
      if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted){out.error=E::encrypted_requires_authority;return out;}
      if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required){out.error=E::cluster_requires_authority;return out;}
      if(!read(offset,image.data(),size)){out.error=E::io_failure;return out;}
      auto after=device.Size();out.io_status=after.status;out.io_diagnostic=std::move(after.diagnostic);
      if(!after.ok()){out.error=E::io_failure;return out;}
      if(before.size_bytes!=after.size_bytes){out.error=E::page_zero_failure;out.page_zero_error=disk::FilespacePageZeroError::invalid_capacity;return out;}
      const disk::NativeCommonPageHeaderBinding expected_page{expected,target.page_number,target.page_generation,6,{}};
      const auto header=disk::DecodeNativeCommonPageHeader(image.data(),128,&expected_page);
      if(!header.ok()){out.error=E::binding_mismatch;out.header_error=header.error;return out;}
      if(header.header->flags&1u){out.error=E::encrypted_requires_authority;return out;}
      if(header.header->flags&2u){out.error=E::cluster_requires_authority;return out;}
      if(header.header->flags&12u){out.error=E::header_policy_requires_authority;return out;}
      auto decoded=DecodeNativeCatalogLeafInto(image,backing.rows_,backing.metadata_);
      if(!decoded.ok()){out.error=E::leaf_failure;out.leaf=std::move(decoded);return out;}
      if(decoded.page->body.relation_uuid.value!=ref.object_uuid){out.error=E::binding_mismatch;return out;}
      out.leaf=std::move(decoded);out.image=image;out.error=E::none;return out;
    }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
     catch(const std::length_error&){out.error=E::resource_exhausted;}
     catch(const std::system_error&){out.error=E::lock_failure;}
    return out;
  }
};
} // namespace scratchbird::storage::database
