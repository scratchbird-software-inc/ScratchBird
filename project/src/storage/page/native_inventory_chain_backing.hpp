// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include "transaction_state_page.hpp"
#include <memory_resource>
namespace scratchbird::storage::page::detail {
// The in-place transaction inventory (SB_COMPACT_TRANSACTION_STATE_PAGE,
// SB_TRANSACTION_HEADER_PAGE), read and checked read-only
// (transaction_inventory_reader.hpp) into the chain-view shape: one page view
// per state page, then one per header page, each with its own common page
// header and the head's object identity (the inventory object). An in-place
// inventory has no per-page generation or digest; each page reports generation
// 1, the constructor's. A current read only: in-place pages have no historical
// image. Backing comes from `resource`; nothing is written and no lock is taken.
// All backing is already admitted; exact-device batches outlive every enclosing
// device fence. This confers no memory, transaction or retention authority.
//
// Every refusal is the reader's own. W4d-3 (2026-11-08): the SBTINV01 chain
// reader that unplaceable heads were once left to is removed, so this reader
// no longer hands anything on: unusable devices or batches are invalid_backing;
// a root that is not kind 4 with the inventory page type, or that names no
// supplied filespace or another page-size profile, is invalid_reference; a head
// page that is not an in-place transaction-state page (family SBTIPW01) is
// invalid_family.
struct NativeInPlaceInventoryRead {
  NativeInventoryChainDeviceRead read;
  // The counters and horizons the first state page stores, present when the
  // read succeeds. read.chain.horizons are those projected from the entries.
  std::optional<TipCounters> counters;
};
NativeInPlaceInventoryRead ReadNativeInPlaceInventoryBacked(
    const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,
    const disk::FilespaceRootReference&,u64 maximum_retained_image_bytes,
    std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::page::detail
