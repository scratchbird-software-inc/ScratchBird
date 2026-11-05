// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_common_page_header.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace scratchbird::storage::page {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// SEARCH_KEY: SB_COMPACT_TRANSACTION_STATE_PAGE
// Owner rulings 2026-11-05: careful write, no intent records, no whole-page
// digest on the transaction inventory; all six horizons are stored; the
// transaction_order is the order transactions were started. This is the
// Firebird-style in-place transaction inventory page (page type 0x0301,
// family "SBTIPW01"): one 4-byte state word per transaction (inventory spec
// §7), indexed by transaction_order. A commit or rollback rewrites one state
// byte; allocation rewrites one aligned u64 counter. Every in-place change
// lies inside one 512-byte write unit, and its old and new values are both
// valid, so a torn write cannot produce an invalid page.
//
// Layout (P = page size):
//   0..127    common page header; its generation is fixed when the page is
//             formatted and never rewritten
//   128..255  family header, fixed after format: magic, version, word size,
//             slots per page, page sequence in the chain, first number
//   256..511  mutable counters, each an aligned u64: the next page in the
//             chain (every page); on the first page only, Next and the six
//             horizons OIT, OAT, OST, OCT, OAR, OLR
//   512..P    state words; slot i holds transaction_order first_number + i
//
// This is a codec only: no I/O, no lifecycle rules. Which transitions are
// legal, chain walking, allocation-map checks and the cross-check against the
// transaction headers belong to the reader and the transaction layer.

inline constexpr u32 kTransactionStatePageType = 0x0301;
inline constexpr std::array<byte, 8> kTransactionStatePageMagic{
    {'S', 'B', 'T', 'I', 'P', 'W', '0', '1'}};
inline constexpr u16 kTransactionStatePageVersion = 1;
inline constexpr std::size_t kTransactionStateFamilyOffset = 128;
inline constexpr std::size_t kTransactionStateCountersOffset = 256;
inline constexpr std::size_t kTransactionStateWordsOffset = 512;
inline constexpr std::size_t kTransactionStateWordBytes = 4;
// The device write unit every in-place change is confined to.
inline constexpr std::size_t kTransactionStateWriteUnitBytes = 512;

// Durable state codes, in the order of the inventory spec §7 list. This is
// the on-disk vocabulary; it is deliberately not TransactionState, whose
// in-memory states differ (created, preparing, failed_terminal). The reader
// maps between the two with an explicit table.
enum class TipStateCode : std::uint8_t {
  unused = 0,
  starting = 1,
  active = 2,
  read_only_active = 3,
  prepared = 4,
  committing = 5,
  committed = 6,
  rolling_back = 7,
  rolled_back = 8,
  dead = 9,
  limbo = 10,
  recovering = 11,
  archived = 12,
};
inline constexpr std::uint8_t kTipStateCodeMax = 12;
const char* TipStateCodeName(TipStateCode code) noexcept;

enum class TipAuthorityClass : std::uint8_t { local = 0, cluster = 1, system = 2 };

// State word flag bits (inventory spec §7). Bits 5-7 are reserved and zero.
inline constexpr std::uint8_t kTipFlagHasDetailRecord = 1u << 0;
inline constexpr std::uint8_t kTipFlagHasLimboRecord = 1u << 1;
inline constexpr std::uint8_t kTipFlagHasArchiveDependency = 1u << 2;
inline constexpr std::uint8_t kTipFlagHasCleanupQueueEntry = 1u << 3;
inline constexpr std::uint8_t kTipFlagHasErrorOrEvidence = 1u << 4;
inline constexpr std::uint8_t kTipFlagMask = 0x1f;
// The intent summary is 6 bits.
inline constexpr std::uint8_t kTipIntentMax = 63;

// byte 0 state code; byte 1 flags; byte 2 authority class (bits 0-1) and
// intent summary (bits 2-7); byte 3 visibility-epoch delta (0 = the
// transaction header carries the reference). An unused word is all zero.
struct TipStateWord {
  TipStateCode state = TipStateCode::unused;
  std::uint8_t flags = 0;
  TipAuthorityClass authority = TipAuthorityClass::local;
  std::uint8_t intent = 0;
  std::uint8_t epoch_delta = 0;
  bool operator==(const TipStateWord&) const = default;
};

// Every horizon is a transaction_order in [1, Next] (inventory spec §12: a
// horizon with nothing to hold back equals Next).
struct TipHorizons {
  u64 oldest_interesting = 1;      // OIT
  u64 oldest_active = 1;           // OAT
  u64 oldest_snapshot = 1;         // OST
  u64 oldest_cleanup = 1;          // OCT
  u64 oldest_archive_required = 1; // OAR
  u64 oldest_limbo_resolution = 1; // OLR
  bool operator==(const TipHorizons&) const = default;
};
// Carried by the first page of the chain only. Next is the next
// transaction_order to allocate; 0 is reserved, so Next >= 1.
struct TipCounters {
  u64 next_transaction = 1;
  TipHorizons horizons;
  bool operator==(const TipCounters&) const = default;
};

enum class TipPageError : std::uint8_t {
  none,
  invalid_page_size,
  invalid_header,
  invalid_family,
  invalid_counters,
  invalid_word,
  invalid_slot,
  invalid_link,
  next_regression,
  short_buffer,
};
const char* TipPageErrorName(TipPageError error) noexcept;

// Slots per page, or 0 when the page size cannot hold a state page (it must
// be a multiple of the write unit and larger than the fixed 512 bytes).
u32 TipSlotsPerPage(u32 page_size) noexcept;

struct TipPageFormat {
  // page_type must be kTransactionStatePageType; flags must be 0.
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 tip_sequence = 0;       // 0 = the first page of the chain
  u64 next_tip_page = 0;      // page number in the same filespace; 0 = none
  // Required on the first page, refused on any other.
  std::optional<TipCounters> counters;
};

// Writes a freshly formatted page (every slot unused) into the first
// page_size bytes of output. Output is untouched on a refusal.
TipPageError FormatTransactionStatePage(const TipPageFormat& format,
                                        std::span<byte> output) noexcept;

struct TipPageView {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  u64 tip_sequence = 0;
  u64 first_number = 0;
  u32 slots = 0;
  u64 next_tip_page = 0;
  std::optional<TipCounters> counters;
  std::span<const byte> image;  // borrowed from the decoded bytes
};
struct TipPageDecodeResult {
  TipPageError error = TipPageError::invalid_header;
  std::optional<TipPageView> view;
  // Slot of the first refused word, for invalid_word / invalid_slot.
  std::optional<u32> slot;
  bool ok() const noexcept { return error == TipPageError::none && view.has_value(); }
};
// Structural decode of one page image (no I/O). `expected` binds the common
// header as DecodeNativeCommonPageHeader does. Checks: header and type;
// family fields agree with the page size and sequence; reserved bytes zero;
// counters only on the first page and every horizon in [1, Next]; every word
// valid; transaction_order 0 never used; on the first page, no used slot at or
// beyond Next. An unused slot below Next is legal: it is what a crash between
// allocating a number and writing its slot leaves, and restart marks it dead.
TipPageDecodeResult DecodeTransactionStatePage(
    std::span<const byte> image,
    const scratchbird::storage::disk::NativeCommonPageHeaderBinding* expected = nullptr) noexcept;

// For a page other than the first: refuses any used slot at or beyond Next
// (the chain reader supplies Next from the first page).
TipPageDecodeResult CheckTransactionStateSlotsBelowNext(const TipPageView& view,
                                                        u64 next_transaction) noexcept;

std::optional<TipStateWord> DecodeTipStateWord(std::span<const byte, kTransactionStateWordBytes> bytes) noexcept;
std::optional<std::array<byte, kTransactionStateWordBytes>> EncodeTipStateWord(const TipStateWord& word) noexcept;
// The word for a slot of a decoded page; nullopt when the slot is out of range.
std::optional<TipStateWord> TipWordAt(const TipPageView& view, u32 slot) noexcept;

// Where transaction_order n lives: page n / slots, slot n % slots.
struct TipSlotLocation {
  u64 tip_sequence = 0;
  u32 slot = 0;
};
std::optional<TipSlotLocation> LocateTransactionStateSlot(u32 page_size, u64 transaction_order) noexcept;

// In-place edits of a page image held by the caller. Each changes bytes inside
// exactly one 512-byte write unit and reports that unit, with its bytes before
// the change, so the caller can add it to a CarefulWritePlan as
// (page_byte_offset + unit_offset, image[unit], preimage). On a refusal the
// image is unchanged. A no-op edit (the value is already there) succeeds with
// changed = false. The image must be a page these functions or the formatter
// produced; its family header is re-checked, the rest is trusted.
struct TipPageEdit {
  TipPageError error = TipPageError::invalid_family;
  u32 unit_offset = 0;
  bool changed = false;
  std::array<byte, kTransactionStateWriteUnitBytes> preimage{};
  bool ok() const noexcept { return error == TipPageError::none; }
};
// Writes a whole state word (BEGIN: unused -> starting with its flags).
TipPageEdit SetTransactionStateWord(std::span<byte> image, u32 slot, const TipStateWord& word) noexcept;
// Rewrites only the state byte (commit, rollback, dead-marking). An unused
// slot keeps its other bytes zero, so only state codes may be written there.
TipPageEdit SetTransactionStateCode(std::span<byte> image, u32 slot, TipStateCode code) noexcept;
// First page only. Next never decreases (transaction numbers are never
// reused, inventory spec §4); every horizon must lie in [1, Next].
TipPageEdit SetTransactionStateCounters(std::span<byte> image, const TipCounters& counters) noexcept;
// Links the next page of the chain. The link is written once: a different
// existing link is refused; the same value is a no-op.
TipPageEdit SetTransactionStateNextPage(std::span<byte> image, u64 next_tip_page) noexcept;

}  // namespace scratchbird::storage::page
