// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include "transaction_header_page.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state_page.hpp"

#include <optional>
#include <utility>
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
//  - a used slot whose word says it has a header has one, and every used
//    slot other than dead has one (BEGIN writes the header before the slot;
//    only restart dead-marking of a never-written slot leaves a used slot
//    without a header);
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

// The in-memory inventory the existing transaction layer works with, built
// from checked images (call CheckTransactionInventoryImages first). One entry
// per used slot below Next that has a header, in transaction_order:
//  - identity: local id = transaction_order, the header's UUID, cluster
//    scope for cluster authority, local otherwise;
//  - state through the explicit durable -> in-memory map; archived takes its
//    origin from the header's final state;
//  - begin time from the UUIDv7 time prefix; final time from the header's
//    completion time;
//  - commit sequences 1..k assigned to committed transactions in commit-time
//    order (ties by transaction_order); the durable order is the commit time.
// A dead slot without a header is skipped: no transaction ever ran under it.
struct TransactionInventoryBuild {
  TipPageError error = TipPageError::invalid_header;
  std::optional<u64> transaction_order;  // the entry that could not be built
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  bool ok() const noexcept { return error == TipPageError::none; }
};
TransactionInventoryBuild BuildLocalTransactionInventoryFromImages(u32 page_size, std::span<const byte> state_images,
                                                                   std::span<const byte> header_images);

// The same build into caller-owned scratch, with no heap allocation (for the
// bounded selected read). Every span needs at least Next - 1 elements (one per
// possible transaction below Next). The entries are written in place; the
// result's view borrows them.
struct TransactionInventoryBuildScratch {
  std::span<scratchbird::transaction::mga::TransactionInventoryEntry> entries;
  std::span<std::pair<u64, std::size_t>> commits;  // (commit time, entry index)
  std::span<std::size_t> indices;                  // validator scratch
  std::span<byte> markers;                         // validator scratch
};
struct TransactionInventoryBuildView {
  TipPageError error = TipPageError::invalid_header;
  std::optional<u64> transaction_order;
  u64 next_local_transaction_id = 0, next_commit_sequence = 0;
  std::span<scratchbird::transaction::mga::TransactionInventoryEntry> entries;
  bool ok() const noexcept { return error == TipPageError::none; }
};
TransactionInventoryBuildView BuildLocalTransactionInventoryInto(u32 page_size, std::span<const byte> state_images,
                                                                 std::span<const byte> header_images,
                                                                 const TransactionInventoryBuildScratch& scratch) noexcept;

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
  // Every byte actually transferred, including pages read before a refusal
  // and a failing read's partial transfer.
  u64 physical_bytes_read = 0;
  bool ok() const noexcept { return !io_failure && check.ok(); }
};
// Reads both chains from the open devices and runs the check. On success the
// images stay in the backing for the caller (for example the restart pass).
TransactionInventoryRead ReadTransactionInventory(const TransactionInventoryReadRequest& request) noexcept;

// SB_TRANSACTION_INVENTORY_WRITER (owner Q20 = 1, 2026-11-07): the horizons
// on the first state page are written when BEGIN allocates a number and are
// never lowered; COMMIT, ROLLBACK and restart dead-marking do not rewrite them
// (FirebirdSQL recomputes them at the next start). After any of those, or a
// crash between BEGIN's writes, they trail what the inventory projects. A
// reader admits them when the stored Next equals the inventory's Next and each
// stored OIT, OAT and OST is at most the projected value: a stored horizon may
// hold back more than needed, never less. The codec already keeps every
// stored horizon in [1, Next].
bool StoredTransactionHorizonsAdmissible(const TipCounters& stored, u64 inventory_next,
                                         const scratchbird::transaction::mga::LocalTransactionHorizons& projected) noexcept;

}  // namespace scratchbird::storage::page
