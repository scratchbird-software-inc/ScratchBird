// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_readonly.hpp"

namespace scratchbird::engine::internal_api {
// Pure comparison, never an allocation grant. The owning reader supplies the
// actual selected chain and root, then binds this exact physical page UUID and
// creator to the bytes it reads before retaining any verification observation.
inline std::optional<storage::page::NativeAllocationRecord> FindNativeStartupRootAllocation(
    const storage::database::NativeReadAllocationChain& chain,
    const storage::database::NativeCheckpointRootReference& root) noexcept {
  if(!chain.ok())return {};
  std::optional<storage::page::NativeAllocationRecord> found;
  for(const auto& image:chain.pages){const auto& map=image.map;
    if(map.header.filespace_uuid!=root.page.filespace_uuid||map.header.page_size_profile_uuid!=root.page.page_size_profile_uuid)continue;
    for(const auto& record:map.records)if(record.page_number==root.page.page_number){
      if(found||record.page_number<map.first_page||record.page_number-map.first_page>=map.states.size()||
          map.states[record.page_number-map.first_page]!=storage::page::NativeAllocationState::allocated||
          record.page_generation!=root.page.page_generation||record.page_type!=root.page_type||record.owner_uuid!=root.object_uuid)return {};
      found=record;
    }
  }
  return found;
}
} // namespace scratchbird::engine::internal_api
