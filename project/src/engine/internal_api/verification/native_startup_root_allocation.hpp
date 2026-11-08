// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_page_chain_readonly.hpp"

namespace scratchbird::engine::internal_api {
// Pure comparison, never an allocation grant. The owning reader supplies the
// actual selected chain and root, then binds this exact physical page UUID and
// creator to the bytes it reads before retaining any verification observation.
inline std::optional<storage::page::NativeAllocationRecord> FindNativeStartupPageAllocation(
    const storage::database::NativeReadAllocationChain& chain,const storage::disk::NativePageReference& page,
    core::platform::u32 page_type,const core::platform::Uuid& owner) noexcept {
  if(!chain.ok())return {};
  std::optional<storage::page::NativeAllocationRecord> found;
  for(const auto& image:chain.pages){const auto& map=image.map;
    if(map.header.filespace_uuid!=page.filespace_uuid||map.header.page_size_profile_uuid!=page.page_size_profile_uuid)continue;
    for(const auto& record:map.records)if(record.page_number==page.page_number){
      if(found||record.page_number<map.first_page||record.page_number-map.first_page>=map.states.size()||
          map.states[record.page_number-map.first_page]!=storage::page::NativeAllocationState::allocated||
          record.page_generation!=page.page_generation||record.page_type!=page_type||record.owner_uuid!=owner)return {};
      found=record;
    }
  }
  return found;
}
// SB_NATIVE_FIXED_ROOT_READ: a root page zero names.
inline std::optional<storage::page::NativeAllocationRecord> FindNativeStartupRootAllocation(
    const storage::database::NativeReadAllocationChain& chain,const storage::disk::FilespaceRootReference& root) noexcept {
  return FindNativeStartupPageAllocation(chain,{root.filespace_uuid,root.page_number,root.page_generation,root.page_size_profile_uuid},
      root.page_type,root.object_uuid);
}
} // namespace scratchbird::engine::internal_api
