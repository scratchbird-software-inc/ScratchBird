// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include "transaction_header_page.hpp"
#include "transaction_state_page.hpp"

#include <optional>
#include <span>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_TRANSACTION_INVENTORY_READER
// Read-only load and verification of the in-place transaction inventory: the
// transaction-state chain (SBTIPW01) and the transaction header chain
// (SBTXHD01) it points to. Owner rulings 2026-11-05: open verification is
// read-only, records nothing and replays nothing; the database file is
// authoritative. Nothing is written and no lock is taken (MGA: only
// write-write blocks).
//
// Checks, beyond each page's own decode:
//  - each chain starts at sequence 0 and advances by one per page, every page
//    in the same filespace, database and page size;
//  - the state chain covers every transaction_order below Next, and no used
//    slot is at or beyond Next;
//  - no header exists at or beyond Next (BEGIN allocates Next before writing
//    the header);
//  - a used slot whose word says it has a header has one;
//  - committed or rolled_back slots have a header completed the same way
//    (the header is written before the state byte); archived slots have a
//    completed header; prepared and limbo slots have an incomplete header.
// A header whose slot is unused, or a completed header whose slot still says
// in progress or dead, is the crash window between two careful writes and is
// legal; restart dead-marking resolves the slot.

enum class TransactionInventoryReadStage : std::uint8_t { none, request, state_chain, header_chain, cross_check };

struct TransactionInventoryCheck {
  TipPageError error = TipPageError::invalid_header;
  TransactionInventoryReadStage stage = TransactionInventoryReadStage::none;
  std::size_t page_index = 0;         // index within the failing chain
  std::optional<u64> transaction_order;  // for slot/record findings
  TipCounters counters;               // valid on success
  bool ok() const noexcept { return error == TipPageError::none; }
};

// Pure check over page images already in memory, each run a whole chain in
// order, page after page: `state_images` the state chain, `header_images`
// the header chain. Sizes must be whole multiples of page_size.
TransactionInventoryCheck CheckTransactionInventoryImages(u32 page_size, std::span<const byte> state_images,
                                                          std::span<const byte> header_images) noexcept;

struct TransactionInventoryReadRequest {
  scratchbird::core::platform::Uuid database_uuid;
  // Page-zero root kind 4: the first transaction-state page.
  scratchbird::storage::disk::FilespaceRootReference state_root;
  std::span<const scratchbird::storage::disk::NativeFilespaceDevice> files;
  // One per file, bound to that file's device.
  std::span<scratchbird::storage::disk::FileDevice::ReadLatencyBatch* const> batches;
  // Page images are read into this, state pages first, then header pages;
  // its size bounds how many pages can be read.
  std::span<byte> backing;
};
struct TransactionInventoryRead {
  TransactionInventoryCheck check;
  bool io_failure = false;
  scratchbird::storage::disk::BoundedIoResult io;  // the failing read, when io_failure
  u32 page_size = 0;
  std::size_t state_pages = 0;   // images at backing[0 .. state_pages * page_size)
  std::size_t header_pages = 0;  // images after the state pages
  bool ok() const noexcept { return !io_failure && check.ok(); }
};
// Reads both chains from the open devices and runs the check. On success the
// images stay in the backing for the caller (for example the restart pass).
TransactionInventoryRead ReadTransactionInventory(const TransactionInventoryReadRequest& request) noexcept;

}  // namespace scratchbird::storage::page
