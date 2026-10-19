// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include <algorithm>
#include <utility>

namespace scratchbird::engine::internal_api {
enum class NativeStartupCatalogAllocationError {none,source_required,invalid_record,orphan_page};
struct NativeStartupCatalogAllocationCheck {
  NativeStartupCatalogAllocationError error=NativeStartupCatalogAllocationError::source_required;
  std::size_t matched_pages=0;
  bool ok()const noexcept{return error==NativeStartupCatalogAllocationError::none;}
};
// Pure comparison of captured physical identities and one selected allocation
// chain. It grants no scan/phase authority. The owner must supply every actual
// filespace and reconcile the total matched count before retaining success.
inline NativeStartupCatalogAllocationCheck CheckNativeStartupCatalogAllocation(
    const storage::database::NativeCatalogCapture& capture,
    const storage::database::NativeReadAllocationChain& chain) noexcept {
  using E=NativeStartupCatalogAllocationError;
  if(!capture.has_physical_pages()||!chain.ok())return {};
  const auto expected=capture.physical_pages();std::size_t matched=0;
  for(const auto& page:chain.pages){const auto& map=page.map;
    for(const auto& record:map.records){
      if(record.page_number<map.first_page||record.page_number-map.first_page>=map.states.size())return {E::invalid_record};
      const auto at=std::lower_bound(expected.begin(),expected.end(),std::pair{map.header.filespace_uuid,record.page_number},
          [](const auto& candidate,const auto& key){return candidate.header.filespace_uuid<key.first||
              (candidate.header.filespace_uuid==key.first&&candidate.header.page_number<key.second);});
      if(at==expected.end()||at->header.filespace_uuid!=map.header.filespace_uuid||at->header.page_number!=record.page_number){
        // First-create has no admitted user record/index graph. A page outside
        // the actually visited catalog graphs is not an empty relation.
        if(record.page_type==5||record.page_type==6||(record.page_type>=0x100&&record.page_type<0x300))return {E::orphan_page};
        continue;
      }
      const auto& h=at->header;
      if(h.database_uuid!=map.header.database_uuid||h.page_size_profile_uuid!=map.header.page_size_profile_uuid||
          h.page_size_bytes!=map.header.page_size_bytes||h.page_type!=record.page_type||h.page_uuid!=record.page_uuid||
          h.page_generation!=record.page_generation||at->owner_uuid!=record.owner_uuid||
          map.states[record.page_number-map.first_page]!=storage::page::NativeAllocationState::allocated)return {E::invalid_record};
      ++matched;
    }
  }
  return {E::none,matched};
}
} // namespace scratchbird::engine::internal_api
