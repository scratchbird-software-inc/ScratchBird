// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "session_ledger_record.hpp"

#include <array>
#include <optional>
#include <span>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_SESSION_LEDGER_PAGE
// Plan 36 T2 (owner Q165 = 1). The page family of the session ledger chain,
// "SBSESL01", under the transaction inventory page type 0x0301 as SBTXHD01 is
// (the families are told apart by the magic at the family offset). The chain's
// first page is named by page-zero root kind 25 (SB_SESSION_LEDGER_ROOT).
//
// Layout (P = page size), the SBTXHD01 geometry:
//   0..127    common page header (type 0x0301, flags 0)
//   128..255  family header: magic, version, record size (512; OLD 256), slots per
//             page, sequence in the chain, first ordinal; the rest zero
//   256..511  aligned u64 link to the next page of the chain; the rest zero
//   512..P    512-byte ledger records (OLD 256); an empty slot is all zero
// Slots per page: (P - 512) / 512 = 15, 31, 63, 127, 255 at 8, 16, 32, 64,
// 128 KiB (OLD, 256-byte records: 30, 62, 126, 254, 510; owner 2026-11-23).
// Slot i of page s holds the record of ordinal s * slots + i. A record IS one
// 512-byte write unit (it starts on a unit boundary); an append writes one unit.
//
// APPEND-ONLY, no counter. The records of a page fill from slot 0 without a
// gap. A page is formatted empty and linked from the previous page only once
// that page is full, so every page but the last is full. A crash during an
// append can leave ONE torn slot (a torn write unit): bytes that are not all zero and fail the
// record checksum, directly after the last complete record, with every later
// slot zero. That slot is IGNORED (never a visible record) and the next append
// overwrites it (its preimage is the torn bytes). Anything else that is not a
// complete record, or any non-zero slot after a torn or empty one, is
// corruption and is refused with a specific code.

inline constexpr std::array<byte, 8> kSessionLedgerPageMagic{{'S', 'B', 'S', 'E', 'S', 'L', '0', '1'}};
inline constexpr u16 kSessionLedgerPageVersion = 1;
inline constexpr std::size_t kSessionLedgerPageRecordsOffset = 512;

// Slots per page, or 0 when the page size cannot hold a ledger page.
u32 SessionLedgerSlotsPerPage(u32 page_size) noexcept;

struct SessionLedgerSlotLocation {
  u64 sequence = 0;
  u32 slot = 0;
};
std::optional<SessionLedgerSlotLocation> LocateSessionLedgerSlot(u32 page_size, u64 ordinal) noexcept;

struct SessionLedgerPageFormat {
  // page_type must be kTransactionStatePageType; flags must be 0.
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 sequence = 0;
  u64 next_page = 0;  // 0 = none
};
// Writes a freshly formatted page (every slot empty). Output untouched on a refusal.
TipPageError FormatSessionLedgerPage(const SessionLedgerPageFormat& format, std::span<byte> output) noexcept;

struct SessionLedgerPageView {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 sequence = 0;
  u64 first_ordinal = 0;
  u32 slots = 0;
  u64 next_page = 0;
  u32 used = 0;                 // complete records, slots 0..used-1
  std::optional<u32> torn_slot;  // the torn slot (== used) when one is present
  std::span<const byte> image;
};
struct SessionLedgerPageDecodeResult {
  TipPageError error = TipPageError::invalid_header;
  std::optional<SessionLedgerPageView> view;
  std::optional<u32> slot;  // first refused slot, for invalid_word / invalid_slot
  SessionLedgerRecordError record_error = SessionLedgerRecordError::none;  // detail of invalid_word
  bool ok() const noexcept { return error == TipPageError::none && view.has_value(); }
};
// Structural decode: header and type; family fields agree with the page size
// and sequence; reserved bytes zero; slots are complete records (each bound to
// its ordinal and the page's database) then at most one torn slot then zeros.
SessionLedgerPageDecodeResult DecodeSessionLedgerPage(
    std::span<const byte> image,
    const scratchbird::storage::disk::NativeCommonPageHeaderBinding* expected = nullptr) noexcept;
// True for an image that carries the ledger page magic at the family offset
// (no other validation).
bool IsSessionLedgerPageImage(std::span<const byte> image) noexcept;

// The complete record in a slot; nullopt for an empty or torn slot or one out of range.
std::optional<SessionLedgerRecord> SessionLedgerRecordAt(const SessionLedgerPageView& view, u32 slot) noexcept;

struct SessionLedgerAppendEdit : TipPageEdit {
  u32 slot = 0;  // the slot written
};
// Writes `record` (its ordinal must be this page's next ordinal) into the first
// slot that is not a complete record: empty, or the torn one. Refused when the
// page is full or has an unreadable slot. Writing the identical complete record
// again is a no-op.
SessionLedgerAppendEdit AppendSessionLedgerRecord(std::span<byte> image, const SessionLedgerRecord& record) noexcept;
// Links the next page of the chain; written once, and only on a full page.
TipPageEdit SetSessionLedgerNextPage(std::span<byte> image, u64 next_page) noexcept;

}  // namespace scratchbird::storage::page
