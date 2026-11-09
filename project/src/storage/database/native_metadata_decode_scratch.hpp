// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_metadata_memory.hpp"
#include "native_allocation_map.hpp"
#include "transaction_inventory_page.hpp"
#include "native_filespace_directory.hpp"
#include "database_dirty_manifest.hpp"

namespace scratchbird::storage::database::detail {
using namespace core::platform;
using Image=std::span<const byte>;
using disk::detail::NativeMetadataMemory;
using disk::detail::NativeMetadataHeapMemory;
using disk::detail::ReadNativeMetadataPageZero;
struct NativeMetadataScratch:disk::detail::NativeMetadataScratch {
  using disk::detail::NativeMetadataScratch::NativeMetadataScratch;
  auto Directory(Image raw){
    const auto count=raw.size()>=384?std::min<std::size_t>(LoadLittle32(raw.data()+208),(raw.size()-384)/192):0;
    return page::DecodeNativeFilespaceDirectoryInto(raw,Array<page::NativeFilespaceDirectoryRecord>(count),Array<Uuid>(count));
  }
  auto PageZero(Image raw){return disk::DecodeFilespacePageZeroInto(raw,Array<disk::FilespaceRootReference>(32));}

};
} // namespace scratchbird::storage::database::detail
