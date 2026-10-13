// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_catalog_input.hpp"
#include "row_data_page.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database::detail {
struct NativeCreationRelationLayout {
  std::vector<std::size_t> starts;
  std::vector<u64> bases, navigation;
  u64 root=0;
};
struct NativeCreationCatalogLayout {
  std::array<NativeCreationRelationLayout,6> relations;
  u64 image_count=0,root_cells=0,leaf_cells=0;
};
enum class NativeCreationLayoutError {none,invalid_input,capacity_exceeded,resource_exhausted};
struct NativeCreationLayoutResult {
  NativeCreationLayoutError error=NativeCreationLayoutError::invalid_input;
  std::optional<NativeCreationCatalogLayout> layout;
  bool ok() const noexcept{return error==NativeCreationLayoutError::none&&layout.has_value();}
};
// Shared exact physical layout for construction and original-operation recovery.
// No identities, authorization, memory grants, writes or configuration defaults.
inline NativeCreationLayoutResult PlanNativeCreationCatalogLayout(
    u64 size,u64 maps,u64 total,u64 budget,const NativeCreationCatalogSeed& seed,
    const NativeCreationCatalogIndexes& indexes) noexcept {
  using E=NativeCreationLayoutError;
  const auto fail=[](E e){return NativeCreationLayoutResult{e,{}};};
  try {
    if(!std::any_of(disk::kCanonicalFilespacePageProfiles.begin(),disk::kCanonicalFilespacePageProfiles.end(),
        [size](const auto& p){return p.page_size_bytes==size;})||
        maps>std::numeric_limits<u64>::max()-20||total<maps+20)
      return fail(E::invalid_input);
    if(budget/size<2||maps+20>budget/size-2)return fail(E::resource_exhausted);
    NativeCreationCatalogLayout plan;plan.image_count=maps+20;
    plan.root_cells=(size-640)/164;plan.leaf_cells=(size-640-112)/164;
    const u64 capacity=size-128-32-page::kRowDataPageBodyHeaderBytes;
    for(unsigned role=0;role<seed.size();++role) {
      const auto& rows=seed[role];auto& p=plan.relations[role];
      if(rows.size()>(indexes[role]?(plan.root_cells+1)*plan.leaf_cells:size/128))
        return fail(E::capacity_exceeded);
      p.starts.push_back(0);u64 remaining=capacity;
      for(std::size_t at=0;at<rows.size();++at) {
        const auto bytes=NativeCreationCatalogRowBytes(rows[at]);
        if(!bytes)return fail(E::invalid_input);
        if(*bytes>capacity)return fail(E::capacity_exceeded);
        if(*bytes>remaining) {
          if(!indexes[role])return fail(E::capacity_exceeded);
          p.starts.push_back(at);remaining=capacity;
        }
        remaining-=*bytes;
      }
      const auto allocate=[&]() {
        if(plan.image_count>=total)throw E::capacity_exceeded;
        if(plan.image_count>=budget/size-2)throw E::resource_exhausted;
        return plan.image_count++;
      };
      p.bases.push_back(maps+10+role);
      for(std::size_t i=1;i<p.starts.size();++i)p.bases.push_back(allocate());
      if(indexes[role]) {
        p.root=allocate();
        if(rows.size()>plan.root_cells) {
          const auto leaves=(rows.size()+plan.leaf_cells-1)/plan.leaf_cells;
          for(u64 i=0;i<leaves;++i)p.navigation.push_back(allocate());
        }
      }
    }
    return {E::none,std::move(plan)};
  }catch(E e){return fail(e);}
   catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database::detail
