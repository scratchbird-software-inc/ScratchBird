// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_metadata_decode_scratch.hpp"

// SB_DRIFT_REMOVAL_R4A: read-only views of the allocation, directory and
// transaction inventory chains, moved verbatim out of
// native_selected_checkpoint_readonly.hpp so the fixed-root lease and the code
// built on it carry no checkpoint-layer header (owner Q20 = 1, 2026-11-07).
namespace scratchbird::storage::database {
// Immutable projections only; no selection, publication or serving authority.
// The owning lease, its arena and every borrowed device must remain alive.
using NativeReadAllocationMap = page::NativeAllocationMapData<
    std::span<const page::NativeAllocationState>,std::span<const page::NativeAllocationRecord>>;
using NativeReadDirectory = page::NativeFilespaceDirectoryData<
    std::span<const page::NativeFilespaceDirectoryRecord>>;
struct NativeReadTransactionInventory {
  u64 next_local_transaction_id=1,next_commit_sequence=1;
  std::span<const transaction::mga::TransactionInventoryEntry> entries;
};
struct NativeReadAllocationPage {NativeReadAllocationMap map;std::span<const byte> image;};
struct NativeReadAllocationChain {
  page::NativeAllocationError error=page::NativeAllocationError::invalid_reference;
  std::span<const NativeReadAllocationPage> pages;
  std::array<u64,8> state_counts{};
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==page::NativeAllocationError::none&&!pages.empty();}
};
struct NativeReadDirectoryPage {NativeReadDirectory directory;std::span<const byte> image;};
struct NativeReadDirectoryChain {
  page::NativeDirectoryError error=page::NativeDirectoryError::invalid_reference;
  std::span<const NativeReadDirectoryPage> pages;
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==page::NativeDirectoryError::none&&!pages.empty();}
};
// SB_NATIVE_FIXED_ROOT_READ: one inventory page's common header and owning
// object, moved verbatim out of native_checkpoint_root.hpp (deleted with the
// checkpoint layer, W4d); the fixed-root lease reports its state and header
// pages with it.
struct NativeInventoryPageBinding {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::Uuid object_uuid;
};
namespace detail {
inline NativeReadAllocationMap ReadOnly(const page::NativeAllocationMapView& v) noexcept {
  return {v.header,v.object_uuid,v.map_generation,v.capacity_generation,v.total_pages,v.first_page,
    v.creator_transaction_uuid,v.creator_local_transaction_id,v.next,v.next_sha256,
    v.states,v.records,v.creator_operation_uuid};
}
inline NativeReadDirectory ReadOnly(const page::NativeFilespaceDirectoryView& v) noexcept {
  return {v.header,v.object_uuid,v.directory_generation,v.creator_transaction_uuid,
    v.creator_local_transaction_id,v.total_records,v.first_record,v.next,v.next_sha256,
    v.records,v.creator_operation_uuid};
}
inline NativeReadAllocationChain ReadOnly(const page::NativeAllocationChainView& v,NativeMetadataScratch& scratch) {
  auto pages=scratch.Array<NativeReadAllocationPage>(v.pages.size());
  for(std::size_t i=0;i<pages.size();++i)pages[i]={ReadOnly(v.pages[i].map),v.pages[i].image};
  return {v.error,pages,v.state_counts,v.retained_image_bytes,v.backing_bytes_used};
}
inline NativeReadDirectoryChain ReadOnly(const page::NativeDirectoryChainView& v,NativeMetadataScratch& scratch) {
  auto pages=scratch.Array<NativeReadDirectoryPage>(v.pages.size());
  for(std::size_t i=0;i<pages.size();++i)pages[i]={ReadOnly(v.pages[i].directory),v.pages[i].image};
  return {v.error,pages,v.retained_image_bytes,v.backing_bytes_used};
}
} // namespace detail
} // namespace scratchbird::storage::database
