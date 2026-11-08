// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "allocation_map_writer.hpp"
#include "native_common_page_header.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace scratchbird::storage::page {
namespace {
namespace disk = scratchbird::storage::disk;
using E = AllocationWriteError;
using disk::CarefulWriteError;
using S = NativeAllocationState;
constexpr std::size_t kStates = kAllocationMapInplaceStatesOffset;
constexpr std::size_t kUnit = kAllocationMapInplaceWriteUnitBytes;

S SlotState(std::span<const byte> image, u32 slot) {
  return static_cast<S>((image[kStates + slot / 2] >> (4 * (slot % 2))) & 15);
}
bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }
bool RequestValid(const PageAllocationRequest& r) {
  if (!r.page_type || !disk::IsRegisteredNativePageType(r.page_type) || !V7(r.owner_uuid) || !V7(r.page_uuid) ||
      !V7(r.allocation_uuid))
    return false;
  if (r.creator_transaction_uuid.is_nil()) return r.creator_local_transaction_id == 0;
  return V7(r.creator_transaction_uuid) && r.creator_local_transaction_id != 0;
}
}  // namespace

const char* AllocationWriteErrorName(AllocationWriteError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::io_failure: return "io_failure";
    case E::invalid_map: return "invalid_map";
    case E::sealed_map: return "sealed_map";
    case E::filespace_full: return "filespace_full";
    case E::plan_refused: return "plan_refused";
    case E::write_failed: return "write_failed";
  }
  return "unknown";
}

AllocationMapChain::LoadResult AllocationMapChain::Load(disk::CarefulWriteSink& sink,
                                                        const AllocationMapBinding& binding) noexcept {
  LoadResult out;
  try {
    const auto& root = binding.allocation_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(root.page_size_profile_uuid);
    if (binding.database_uuid.is_nil() || root.kind != 3 || root.page_type != 3 || !root.page_number ||
        !root.page_generation || !profile || !binding.maximum_chain_pages)
      return out;
    AllocationMapChain chain;
    chain.page_size_ = profile->page_size_bytes;
    const u32 slots = AllocationMapInplaceSlotsPerPage(chain.page_size_);
    if (!slots) return out;
    std::vector<S> states(slots);
    std::vector<NativeAllocationRecord> records(slots);
    u64 page = root.page_number, generation = root.page_generation, covered = 0;
    for (std::size_t i = 0;; ++i) {
      if (i >= binding.maximum_chain_pages || page > std::numeric_limits<u64>::max() / chain.page_size_) {
        out.error = E::invalid_map;
        out.map_error = NativeAllocationError::chain_mismatch;
        return out;
      }
      const auto at = chain.images_.size();
      chain.images_.resize(at + chain.page_size_);
      const auto image = std::span<byte>(chain.images_).subspan(at, chain.page_size_);
      const auto io = sink.ReadAt(page * chain.page_size_, image);
      if (!io.ok() || io.bytes_transferred != chain.page_size_) {
        out.error = E::io_failure;
        out.io = io;
        return out;
      }
      if (!IsAllocationMapInplaceImage(image)) {
        out.error = i == 0 ? E::sealed_map : E::invalid_map;
        out.map_error = NativeAllocationError::chain_mismatch;
        return out;
      }
      const disk::NativeCommonPageHeaderBinding expected{
          {binding.database_uuid, root.filespace_uuid, root.page_size_profile_uuid}, page, generation, 3, {}};
      const auto decoded = DecodeAllocationMapInplaceInto(image, states, records, &expected);
      if (!decoded.ok()) {
        out.error = E::invalid_map;
        out.map_error = decoded.error;
        return out;
      }
      const auto& view = *decoded.view;
      if (view.sequence != i || view.map.object_uuid != root.object_uuid || view.map.first_page != covered ||
          (i && view.map.total_pages != chain.total_pages_)) {
        out.error = E::invalid_map;
        out.map_error = NativeAllocationError::chain_mismatch;
        return out;
      }
      chain.total_pages_ = view.map.total_pages;
      chain.pages_.push_back(page);
      chain.first_page_.push_back(view.map.first_page);
      chain.slots_.push_back(view.slots);
      covered += view.slots;
      if (!view.next_page) break;
      if (std::find(chain.pages_.begin(), chain.pages_.end(), view.next_page) != chain.pages_.end()) {
        out.error = E::invalid_map;
        out.map_error = NativeAllocationError::chain_mismatch;
        return out;
      }
      page = view.next_page;
      generation = view.next_generation;
    }
    if (covered != chain.total_pages_) {
      out.error = E::invalid_map;
      out.map_error = NativeAllocationError::invalid_range;
      return out;
    }
    out.chain = std::move(chain);
    out.error = E::none;
    return out;
  } catch (const std::bad_alloc&) {
    out.error = E::invalid_request;
    return out;
  } catch (const std::length_error&) {
    out.error = E::invalid_request;
    return out;
  }
}

u64 AllocationMapChain::free_slots() const noexcept {
  u64 count = 0;
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    const auto image = std::span<const byte>(images_).subspan(i * page_size_, page_size_);
    for (u32 slot = 0; slot < slots_[i]; ++slot)
      if (SlotState(image, slot) == S::free) ++count;
  }
  return count;
}

AllocationMapChain::Planned AllocationMapChain::PlanAllocate(disk::CarefulWritePlan& plan,
                                                             const PageAllocationRequest& request) noexcept {
  Planned out;
  if (planned_ || !RequestValid(request)) return out;
  planned_ = true;  // one allocation per plan; the in-memory chain is edited below
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    auto image = std::span<byte>(images_).subspan(i * page_size_, page_size_);
    for (u32 slot = 0; slot < slots_[i]; ++slot) {
      if (SlotState(image, slot) != S::free) continue;
      NativeAllocationRecord record;
      record.page_number = first_page_[i] + slot;
      record.allocation_uuid = request.allocation_uuid;
      record.page_uuid = request.page_uuid;
      record.owner_uuid = request.owner_uuid;
      record.creator_transaction_uuid = request.creator_transaction_uuid;
      record.creator_local_transaction_id = request.creator_local_transaction_id;
      record.page_generation = 1;
      record.page_type = request.page_type;
      const auto written = WriteAllocationMapInplaceRecord(image, slot, record);
      if (!written.ok()) {
        out.error = E::invalid_map;
        out.map_error = written.error;
        return out;
      }
      const auto allocated = SetAllocationMapInplaceAllocated(image, slot);
      if (!allocated.ok()) {
        out.error = E::invalid_map;
        out.map_error = allocated.error;
        return out;
      }
      const u64 base = pages_[i] * page_size_;
      const auto unit = [&](u32 offset) { return std::span<const byte>(image).subspan(offset, kUnit); };
      if (written.unit_offset == allocated.unit_offset) {
        // Record and state in one unit: one write, old and new both valid.
        const auto node = plan.Add(base + written.unit_offset, unit(written.unit_offset),
                                   std::span<const byte>(written.preimage));
        if (!node.ok()) {
          out.error = E::plan_refused;
          return out;
        }
        out.last_node = node.node;
        out.map_writes = 1;
      } else {
        const auto first = plan.Add(base + written.unit_offset, unit(written.unit_offset),
                                    std::span<const byte>(written.preimage));
        const auto second = plan.Add(base + allocated.unit_offset, unit(allocated.unit_offset),
                                     std::span<const byte>(allocated.preimage));
        if (!first.ok() || !second.ok() || plan.Before(first.node, second.node) != CarefulWriteError::none) {
          out.error = E::plan_refused;
          return out;
        }
        out.last_node = second.node;
        out.map_writes = 2;
      }
      out.page_number = record.page_number;
      out.error = E::none;
      return out;
    }
  }
  out.error = E::filespace_full;
  return out;
}

PageAllocationResult AllocatePage(disk::CarefulWriteSink& sink, const AllocationMapBinding& binding,
                                  const PageAllocationRequest& request,
                                  disk::CarefulWriteBarrierPolicy policy) noexcept {
  PageAllocationResult out;
  try {
    if (!RequestValid(request)) return out;
    auto loaded = AllocationMapChain::Load(sink, binding);
    if (!loaded.ok()) {
      out.error = loaded.error;
      out.map_error = loaded.map_error;
      out.io = loaded.io;
      return out;
    }
    disk::CarefulWritePlan plan;
    const auto planned = loaded.chain->PlanAllocate(plan, request);
    if (!planned.ok()) {
      out.error = planned.error;
      out.map_error = planned.map_error;
      return out;
    }
    const auto r = plan.Execute(sink, policy);
    out.write_error = r.error;
    out.io = r.io;
    out.writes = r.written;
    out.syncs = r.syncs;
    if (r.ok()) {
      out.page_number = planned.page_number;
      out.error = E::none;
      return out;
    }
    out.attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                    r.error == CarefulWriteError::barrier_sync_failed ||
                    r.error == CarefulWriteError::final_sync_failed || r.error == CarefulWriteError::sink_exception;
    out.error = out.attempted                                          ? E::write_failed
                : r.error == CarefulWriteError::preimage_read_failed ? E::io_failure
                : r.error == CarefulWriteError::preimage_mismatch    ? E::invalid_map
                                                                     : E::plan_refused;
    return out;
  } catch (...) {
    out.error = E::invalid_request;
    return out;
  }
}

}  // namespace scratchbird::storage::page
