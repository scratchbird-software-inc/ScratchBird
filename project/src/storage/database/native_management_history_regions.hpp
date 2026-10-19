// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_history.hpp"
#include "native_decoded_storage_ranges.hpp"
namespace scratchbird::storage::database::detail {
// Validate all borrowed ranges before a family reader allocates scratch. The
// reader must not overwrite history indexes, nested images or other families.
inline bool DisjointNativeManagementHistoryBacking(const storage::database::NativeManagementHistoryView& history,std::span<byte> backing) noexcept {
  const auto Region=[](auto region){
    using Element=typename decltype(region)::element_type;
    return disk::detail::DecodedRange(region).valid&&
      (region.empty()||reinterpret_cast<std::uintptr_t>(region.data())%alignof(Element)==0);
  };
  const auto safe=[&](auto input){return storage::disk::detail::DisjointNativeDecodeRegions(backing,input);};
  const auto text=[&](std::string_view value){return safe(std::span{value.data(),value.size()});};
  if(!Region(history.entries)||!Region(history.latest)||!Region(history.idempotency)||
      !safe(std::span{&history,1})||!safe(history.entries)||!safe(history.latest)||!safe(history.idempotency))return false;
  for(const auto& key:history.idempotency)if(!text(key.key))return false;
  for(const auto& entry:history.entries){const auto& record=entry.record;
    if(!Region(record.steps)||!Region(entry.extent_pages)||!Region(entry.bundle_pages)||
        !Region(entry.control_allocation_images)||!Region(entry.control_inventory_images)||
        !Region(entry.control_directory_images)||!Region(entry.control_growth_images)||
        !Region(entry.extent_images)||!Region(entry.bundle_images)||
        !safe(record.steps)||!text(record.idempotency_key)||!safe(record.normalized_request_bytes)||!safe(record.operation_state_bytes)||
        !safe(entry.aggregate)||!safe(entry.plan_image)||!safe(entry.extent_images)||!safe(entry.bundle_images)||
        !safe(entry.extent_pages)||!safe(entry.bundle_pages)||!safe(entry.control_allocation_images)||
        !safe(entry.control_inventory_images)||!safe(entry.control_directory_images)||!safe(entry.control_growth_images))return false;
    for(const auto& step:record.steps)if(!text(step.idempotency_key))return false;
    for(const auto images:{entry.control_allocation_images,entry.control_inventory_images,entry.control_directory_images,entry.control_growth_images})
      for(const auto image:images)if(!safe(image))return false;
    for(const auto image:entry.extent_images)if(!safe(image))return false;
    for(const auto image:entry.bundle_images)if(!safe(image))return false;
  }
  return true;
}
} // namespace scratchbird::storage::database::detail
