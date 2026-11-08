// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_allocation_map.hpp"

#include <array>
#include <optional>
#include <span>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_ALLOCATION_MAP_IN_PLACE
// Owner ruling Q22 = 1 (2026-11-07): pages are allocated in place by careful
// write, so the allocation map is an in-place family, like the transaction-
// state pages (SBTIPW01): no whole-page seal, no digest link between map
// pages, and page zero's free/preallocated counters are not cross-checked
// against it (spec annotation SB_DRIFT_ALLOCATION_MAP_SEALED_IMAGE). It keeps
// page type 0x0003 and the sealed family's 128-byte allocation record, so its
// readers produce the same NativeAllocationMapView; the family magic
// ("SBAMIP01") tells the two apart. FirebirdSQL's equivalent is the page
// inventory page (ods.h page_inv_page), which carries one bit per page.
//
// Layout (P = page size). A map page covers N consecutive pages; slot i is
// page first_page + i.
//   0..127    common header; its generation is fixed at format
//   128..255  family header, fixed after format: magic, version, record
//             size (128), N, records offset, object, sequence in the chain,
//             first page, the filespace's total pages, and the map page's
//             creator (a transaction or the creating operation)
//   256..511  mutable: the next map page and its generation (aligned u64s,
//             written once; 0 = last page); the rest zero
//   512..     one 4-bit state per slot, then zero up to the records offset
//   records   128 bytes per slot, at a 128-byte boundary, so each record lies
//             inside one 512-byte write unit; an empty slot is all zero
// Slots per page: the largest N with roundup128(512 + ceil(N / 2)) + 128 N <= P
// (59 at 8 KiB, 123, 251, 506, 1016 at 128 KiB: one fewer than the sealed
// family, whose header is 384 bytes, and the same number of map pages for the
// page counts the native create uses).
//
// Record rules. A slot whose state is not free has a record whose page number
// is the slot's page. A record over a free slot is what a crash between the
// two writes of an allocation leaves (record first, then state): it is legal,
// ignored by readers and overwritten by the next allocation of that slot. A
// record has at most one creator: a transaction (UUIDv7 and local number), the
// creating operation, or neither, for a page the system allocated for itself
// (owner 2026-11-07: for example a page that grows the transaction chains).
// Every in-place edit lies inside one 512-byte unit and both its old and new
// values are valid, so a torn write of a unit leaves the old or the new map.
// This is a codec only: no I/O, chain walking or allocation policy.

inline constexpr std::array<byte, 8> kAllocationMapInplaceMagic{{'S', 'B', 'A', 'M', 'I', 'P', '0', '1'}};
inline constexpr core::platform::u16 kAllocationMapInplaceVersion = 1;
inline constexpr std::size_t kAllocationMapInplaceWriteUnitBytes = 512;
inline constexpr std::size_t kAllocationMapInplaceStatesOffset = 512;
inline constexpr std::size_t kAllocationMapInplaceRecordBytes = 128;

// True for an image that carries this family's magic at the family offset
// (no other validation).
bool IsAllocationMapInplaceImage(std::span<const byte> image) noexcept;
// Slots per page, or 0 when the page size cannot hold one slot.
u32 AllocationMapInplaceSlotsPerPage(u32 page_size) noexcept;
// Offset of slot 0's record for a page covering `slots` pages.
std::size_t AllocationMapInplaceRecordsOffset(u32 slots) noexcept;

struct AllocationMapInplaceFormat {
  disk::NativeCommonPageHeader header;  // page_type 3, flags 0
  Uuid object_uuid;                     // the allocation root's object
  u64 sequence = 0;                     // 0 = the first map page
  u64 first_page = 0;
  u32 slots = 0;                        // pages covered, 1..AllocationMapInplaceSlotsPerPage
  u64 total_pages = 0;                  // of the filespace
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  Uuid creator_operation_uuid;
  u64 next_page = 0, next_generation = 0;  // both 0 on the last page
};

// Encodes a page with the given slot states and records (records ascending by
// page number, one for every non-free slot and none for a free one). With no
// states given every slot is free. Output (the first page-size bytes) is
// untouched on a refusal.
NativeAllocationError EncodeAllocationMapInplacePage(const AllocationMapInplaceFormat& format,
                                                     std::span<const NativeAllocationState> states,
                                                     std::span<const NativeAllocationRecord> records,
                                                     std::span<byte> output) noexcept;

struct AllocationMapInplaceView {
  NativeAllocationMapView map;  // records: the non-free slots', ascending
  u64 sequence = 0;
  u32 slots = 0;
  u64 next_page = 0, next_generation = 0;
  u32 leftover_records = 0;  // records over free slots (crash leftovers)
};
struct AllocationMapInplaceDecodeResult {
  NativeAllocationError error = NativeAllocationError::invalid_family;
  std::optional<AllocationMapInplaceView> view;
  bool ok() const noexcept { return error == NativeAllocationError::none && view.has_value(); }
};
// Structural decode into caller storage (each span at least the page's slot
// count). `expected` binds the common header. map_generation and
// capacity_generation read as 1 and next_sha256 as zero: this family has no
// generations or digest links. map.next is set from the stored next page.
AllocationMapInplaceDecodeResult DecodeAllocationMapInplaceInto(
    std::span<const byte> image, std::span<NativeAllocationState> states, std::span<NativeAllocationRecord> records,
    const disk::NativeCommonPageHeaderBinding* expected = nullptr) noexcept;

// In-place edits of a page image held by the caller. Each changes bytes inside
// exactly one 512-byte unit and reports the unit with its bytes before the
// change. A no-op edit succeeds with changed = false; a refusal leaves the
// image unchanged. The image must be one this codec produced; its family
// header is re-checked, the rest is trusted.
struct AllocationMapInplaceEdit {
  NativeAllocationError error = NativeAllocationError::invalid_family;
  u32 unit_offset = 0;
  bool changed = false;
  std::array<byte, kAllocationMapInplaceWriteUnitBytes> preimage{};
  bool ok() const noexcept { return error == NativeAllocationError::none; }
};
// Writes the record of a free slot (empty, or holding a leftover record).
// The record's page number must be the slot's page.
AllocationMapInplaceEdit WriteAllocationMapInplaceRecord(std::span<byte> image, u32 slot,
                                                         const NativeAllocationRecord& record) noexcept;
// free -> allocated, only when the slot holds a valid record. (Release and
// reuse belong to sweep and are not edits of this step.)
AllocationMapInplaceEdit SetAllocationMapInplaceAllocated(std::span<byte> image, u32 slot) noexcept;
// Links the next map page; written once.
AllocationMapInplaceEdit SetAllocationMapInplaceNext(std::span<byte> image, u64 next_page,
                                                     u64 next_generation) noexcept;

}  // namespace scratchbird::storage::page
