// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_metadata_memory.hpp"
#include "native_allocation_map.hpp"
#include "transaction_inventory_page.hpp"
#include "native_filespace_directory.hpp"
#include "native_management_operation.hpp"
#include "database_dirty_manifest.hpp"

namespace scratchbird::storage::database::detail {
using namespace core::platform;
using Image=std::span<const byte>;
using disk::detail::NativeMetadataMemory;
using disk::detail::NativeMetadataHeapMemory;
using disk::detail::ReadNativeMetadataPageZero;
struct NativeMetadataScratch:disk::detail::NativeMetadataScratch {
  using disk::detail::NativeMetadataScratch::NativeMetadataScratch;
  auto Map(Image raw){
    const auto available=raw.size()>=384?raw.size()-384:0;
    const auto states=raw.size()>=384?std::min<u64>(LoadLittle64(raw.data()+192),u64{available}*2):0;
    const auto records=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+308),available/128):0;
    return page::DecodeNativeAllocationMapInto(raw,Array<page::NativeAllocationState>(states),Array<page::NativeAllocationRecord>(records));
  }
  auto Inventory(Image raw){
    const auto count=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+184),(raw.size()-384)/72):0;
    return page::DecodeNativeTransactionInventoryPageInto(raw,Array<transaction::mga::TransactionInventoryEntry>(count),
      Array<std::size_t>(count),Array<byte>(count));
  }
  auto Directory(Image raw){
    const auto count=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+208),(raw.size()-384)/192):0;
    return page::DecodeNativeFilespaceDirectoryInto(raw,Array<page::NativeFilespaceDirectoryRecord>(count),Array<Uuid>(count));
  }
  auto PageZero(Image raw){return disk::DecodeFilespacePageZeroInto(raw,Array<disk::FilespaceRootReference>(32));}
  auto Checkpoint(Image raw){return DecodeNativeCheckpointRootInto(raw,Array<NativeCheckpointRootReference>(16));}
  NativeManagementOperationViewWorkspace OperationWorkspace(Image raw){
    const auto count=raw.size()>=512?std::min<std::size_t>(LoadLittle32(raw.data()+456),(raw.size()-512)/256):0;
    return {Array<NativeManagementStepView>(count),Array<Uuid>(2*count+7)};
  }

};
} // namespace scratchbird::storage::database::detail
