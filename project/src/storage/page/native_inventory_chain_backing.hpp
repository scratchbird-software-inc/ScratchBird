// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include <memory_resource>
namespace scratchbird::storage::page::detail {
// Internal complete-source composition. All backing is already admitted and
// input-disjoint; exact-device batches outlive every enclosing device fence.
// This confers no memory, transaction, selection or retention authority.
NativeInventoryChainDeviceRead ReadNativeInventoryChainBacked(
    const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,
    const disk::FilespaceRootReference&,u64,NativeInventoryChainReadContext,
    const std::array<byte,32>*,std::span<const NativeInventoryHistoricalPageZero>,
    std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;

// SB_TRANSITIONAL_LEGACY_INVENTORY (owner 2026-11-05, Q8 = 2): while the
// drift-layer tests still build SBTINV01 databases, the checkpoint-bound
// readers accept both inventory formats through this one reader, so they
// cannot disagree. `handled` is false when the head page is not an in-place
// transaction-state page (family SBTIPW01); the caller then reads the SBTINV01
// chain as before. Otherwise `read` is the in-place inventory, read and checked
// read-only (transaction_inventory_reader.hpp), in the same chain-view shape:
// one page view per state page, then one per header page, each with its own
// common page header and the head's object identity (the inventory object).
// An in-place inventory has no per-page generation or digest; each page
// reports generation 1, the constructor's. A current read only: in-place pages
// have no historical image. Backing comes from `resource`; nothing is written
// and no lock is taken. `head_image`, when given, is a whole-page read of the
// head made by the caller (the SBTINV01 reader's in_place_head_image); it
// stands in for the probe, and the chain's own read of the head must equal
// it byte for byte. A probe is likewise checked against that read, so every
// byte this reader is handed or reads is verified. Deleted with the SBTINV01
// chain.
struct NativeInPlaceInventoryRead {
  bool handled=false;
  NativeInventoryChainDeviceRead read;
};
NativeInPlaceInventoryRead ReadNativeInPlaceInventoryBacked(
    const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,
    const disk::FilespaceRootReference&,u64 maximum_retained_image_bytes,
    std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&,
    std::span<const byte> head_image={}) noexcept;
} // namespace scratchbird::storage::page::detail
