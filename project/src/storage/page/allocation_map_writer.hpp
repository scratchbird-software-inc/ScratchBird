// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "allocation_map_inplace_page.hpp"
#include "careful_write.hpp"
#include "filespace_page_zero.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_ALLOCATION_MAP_IN_PLACE
// Allocation from the in-place allocation map by careful write (owner ruling
// Q22 = 1, 2026-11-07; FirebirdSQL PAG_allocate). An allocation writes, in
// order and each durable before the next:
//   1. the page's 128-byte record into its free slot;
//   2. the slot's state: free -> allocated.
// When the record and the state lie in the same 512-byte unit they are still
// two writes of that unit, in that order (owner Q76 = 1, 2026-11-13; the
// record-before-state order of allocation_map_inplace_page.hpp): first the
// unit with the new record and the old state, durable, then the unit with
// both, so a write torn anywhere leaves a valid map. A plan holds one node per
// extent, so the first is returned in Planned for the caller to run, durable,
// before the plan (PlanAllocationRecordFirst). The caller orders
// what depends on the page after the last map write: the new page's image,
// then the page that links to it (FirebirdSQL: the new page depends on the
// page inventory page; pag.cpp:736). A crash after 1 leaves a record over a
// free slot, which readers ignore and the next allocation of that slot
// overwrites; after 2, an allocated page that nothing reaches yet, which is
// wasted space (owner ruling Q17 = 1) until something links it.
//
// The slot chosen is the lowest free slot of the chain. Pages are never freed
// here (sweep and garbage collection come later), and the filespace is never
// extended (filespace growth comes later): with no free slot the allocation
// is refused as filespace_full with no write. Page zero's free-page counter
// is not maintained: the in-place family is not cross-checked against it.
// The file is authoritative: the chain is read through the sink for every
// plan. Not thread-safe; one writer per database (the caller's custody).

enum class AllocationWriteError : std::uint8_t {
  none,
  invalid_request,   // bad binding or request; nothing read
  io_failure,        // a read failed; nothing written
  invalid_map,       // the chain on disk is not a valid in-place map (W4d-3b: a head
                     // page of any other family, the removed sealed one included,
                     // is an unknown family)
  filespace_full,    // no free slot
  plan_refused,      // the caller's plan refused the nodes (for example an overlapping unit)
  write_failed,      // a write or sync failed (stand-alone allocation only)
};
const char* AllocationWriteErrorName(AllocationWriteError error) noexcept;

struct AllocationMapBinding {
  scratchbird::core::platform::Uuid database_uuid;
  // Page-zero root kind 3: the first allocation map page.
  scratchbird::storage::disk::FilespaceRootReference allocation_root;
  std::size_t maximum_chain_pages = 1u << 16;
};

// What the allocated page will be. The identities are the caller's UUIDv7s.
// The creator is a transaction (UUIDv7 and local number), or neither for a
// page the system allocates for itself.
struct PageAllocationRequest {
  u32 page_type = 0;
  scratchbird::core::platform::Uuid owner_uuid;
  scratchbird::core::platform::Uuid page_uuid;
  scratchbird::core::platform::Uuid allocation_uuid;
  scratchbird::core::platform::Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
};

// The map chain as read from the file, edited in memory while a plan is
// built. One allocation per plan: a second would share the chain's units.
class AllocationMapChain {
 public:
  struct LoadResult;
  static LoadResult Load(scratchbird::storage::disk::CarefulWriteSink& sink, const AllocationMapBinding& binding) noexcept;

  struct Planned {
    AllocationWriteError error = AllocationWriteError::invalid_request;
    NativeAllocationError map_error = NativeAllocationError::none;
    u64 page_number = 0;
    // The last map write: the caller orders the new page after it.
    scratchbird::storage::disk::CarefulWriteNodeId last_node = scratchbird::storage::disk::kCarefulWriteNoNode;
    u32 map_writes = 0;  // always 2 (Q76 = 1)
    // Owner Q76 = 1: when the record shares the state's unit, the record's
    // write (that unit with the new record and the old state, and its
    // preimage) must be durable before the plan runs; empty otherwise, when
    // both writes are in the plan.
    u64 record_first_offset = 0;
    std::vector<byte> record_first_image, record_first_preimage;
    bool record_first() const noexcept { return !record_first_image.empty(); }
    bool ok() const noexcept { return error == AllocationWriteError::none; }
  };
  // Adds the allocation's map writes to `plan` (with their preimages and
  // order) and applies them to this in-memory chain. Nothing is written.
  Planned PlanAllocate(scratchbird::storage::disk::CarefulWritePlan& plan, const PageAllocationRequest& request) noexcept;

  u32 page_size() const noexcept { return page_size_; }
  u64 total_pages() const noexcept { return total_pages_; }
  std::size_t map_pages() const noexcept { return pages_.size(); }
  // Free slots in the chain (a slot holding a leftover record counts as free).
  u64 free_slots() const noexcept;

 private:
  u32 page_size_ = 0;
  u64 total_pages_ = 0;
  std::vector<u64> pages_;        // map page numbers in chain order
  std::vector<u64> first_page_;   // first covered page of each
  std::vector<u32> slots_;        // pages covered by each
  std::vector<byte> images_;      // the map pages back to back
  bool planned_ = false;
};

struct AllocationMapChain::LoadResult {
  AllocationWriteError error = AllocationWriteError::invalid_request;
  NativeAllocationError map_error = NativeAllocationError::none;
  scratchbird::storage::disk::BoundedIoResult io{};  // the failing read
  std::optional<AllocationMapChain> chain;
  bool ok() const noexcept { return error == AllocationWriteError::none && chain.has_value(); }
};

// Adds a planned allocation's record-first write (Q76 = 1) to its own plan;
// refused when the allocation has none.
scratchbird::storage::disk::CarefulWriteAddResult PlanAllocationRecordFirst(
    scratchbird::storage::disk::CarefulWritePlan& plan, const AllocationMapChain::Planned& planned);

// A stand-alone allocation: load, plan, execute. The page is allocated and
// unformatted; what the caller writes into it and links is the caller's.
struct PageAllocationResult {
  AllocationWriteError error = AllocationWriteError::invalid_request;
  NativeAllocationError map_error = NativeAllocationError::none;
  u64 page_number = 0;
  scratchbird::storage::disk::CarefulWriteError write_error = scratchbird::storage::disk::CarefulWriteError::none;
  scratchbird::storage::disk::BoundedIoResult io{};
  u32 writes = 0, syncs = 0;
  // True when a write or sync was attempted before the failure: the file may
  // hold the record (a leftover) or the allocation (an unreached page).
  bool attempted = false;
  bool ok() const noexcept { return error == AllocationWriteError::none; }
};
PageAllocationResult AllocatePage(scratchbird::storage::disk::CarefulWriteSink& sink,
                                  const AllocationMapBinding& binding, const PageAllocationRequest& request,
                                  scratchbird::storage::disk::CarefulWriteBarrierPolicy policy =
                                      scratchbird::storage::disk::CarefulWriteBarrierPolicy::trust_forced_writes) noexcept;

}  // namespace scratchbird::storage::page
