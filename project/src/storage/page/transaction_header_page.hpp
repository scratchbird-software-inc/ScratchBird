// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_header_record.hpp"

#include <array>
#include <optional>
#include <span>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_TRANSACTION_HEADER_PAGE
// Owner decision 2026-11-05 (Q3 = 1): until the header store is chosen by
// measurement, transaction headers live in header slot pages, a second page
// family ("SBTXHD01") under the transaction inventory page type 0x0301. The
// chain is found from the first transaction-state page (its family header
// carries the first header page). Slot i of page s holds the header of
// transaction_order s * slots + i, so a header is found by order with no
// index; finding one by transaction UUID needs a separate lookup.
//
// Layout (P = page size):
//   0..127    common page header (type 0x0301, generation fixed at format)
//   128..255  family header, fixed after format: magic, version, record
//             size (256), slots per page, sequence in the chain, first number
//   256..511  aligned u64 link to the next page of the chain; rest zero
//   512..P    256-byte header records; an empty slot is all zero
// Slots per page: (P - 512) / 256, so 30 at 8 KiB and 62 at 16 KiB. Two
// records share each 512-byte write unit, and every edit stays inside one
// record, so a torn write of a unit leaves each record old or new.
//
// A record is written once, at BEGIN, into an empty slot. Later edits may
// change only its mutable fields (intent and intent generation, final state,
// commit and completion times), and only forward: completion happens once,
// the intent generation only increases.

inline constexpr std::array<byte, 8> kTransactionHeaderPageMagic{{'S', 'B', 'T', 'X', 'H', 'D', '0', '1'}};
inline constexpr u16 kTransactionHeaderPageVersion = 1;
inline constexpr std::size_t kTransactionHeaderPageRecordsOffset = 512;

// Slots per page, or 0 when the page size cannot hold a header page.
u32 TransactionHeaderSlotsPerPage(u32 page_size) noexcept;

struct TransactionHeaderSlotLocation {
  u64 sequence = 0;
  u32 slot = 0;
};
std::optional<TransactionHeaderSlotLocation> LocateTransactionHeaderSlot(u32 page_size,
                                                                         u64 transaction_order) noexcept;

struct TransactionHeaderPageFormat {
  // page_type must be kTransactionStatePageType; flags must be 0.
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 sequence = 0;
  u64 next_page = 0;  // 0 = none
};
// Writes a freshly formatted page (every slot empty). Output untouched on a refusal.
TipPageError FormatTransactionHeaderPage(const TransactionHeaderPageFormat& format,
                                         std::span<byte> output) noexcept;

struct TransactionHeaderPageView {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 sequence = 0;
  u64 first_number = 0;
  u32 slots = 0;
  u64 next_page = 0;
  std::span<const byte> image;
};
struct TransactionHeaderPageDecodeResult {
  TipPageError error = TipPageError::invalid_header;
  std::optional<TransactionHeaderPageView> view;
  std::optional<u32> slot;  // first refused slot, for invalid_word / invalid_slot
  bool ok() const noexcept { return error == TipPageError::none && view.has_value(); }
};
// Structural decode: header and type; family fields agree with the page size
// and sequence; reserved bytes zero; every non-empty slot decodes as a valid
// header record whose transaction_order is the slot's number and whose
// database is the page's database; the slot of order 0 is empty.
TransactionHeaderPageDecodeResult DecodeTransactionHeaderPage(
    std::span<const byte> image,
    const scratchbird::storage::disk::NativeCommonPageHeaderBinding* expected = nullptr) noexcept;
// Families are told apart by magic: true for an image that carries the header
// page magic at the family offset (no other validation).
bool IsTransactionHeaderPageImage(std::span<const byte> image) noexcept;

// The record in a slot; nullopt for an empty slot or a slot out of range.
std::optional<TransactionHeaderRecord> TransactionHeaderAt(const TransactionHeaderPageView& view, u32 slot) noexcept;

// BEGIN: writes a record into its empty slot (the slot is derived from the
// record's transaction_order). Writing the identical record again is a
// no-op; any other existing record is refused.
TipPageEdit WriteTransactionHeader(std::span<byte> image, const TransactionHeaderRecord& record) noexcept;
// Rewrites an existing record whose fixed fields are unchanged; the mutable
// fields may only move forward (see above).
TipPageEdit UpdateTransactionHeader(std::span<byte> image, const TransactionHeaderRecord& record) noexcept;
// Links the next page of the chain; written once.
TipPageEdit SetTransactionHeaderNextPage(std::span<byte> image, u64 next_page) noexcept;

}  // namespace scratchbird::storage::page
