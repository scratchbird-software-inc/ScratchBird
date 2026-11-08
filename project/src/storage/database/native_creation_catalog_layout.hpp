// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_catalog_input.hpp"
#include "row_data_page.hpp"
#include "native_btree_tree_backing.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database::detail {
struct NativeCreationRelationLayout {
  std::vector<std::size_t> starts;
  std::vector<u64> bases, navigation;
  // Branch pages per tree level above the leaves, bottom-up: branches[0] is
  // level 1. Empty for a tree of depth two or less (SB_CATALOG_BTREE_N_LEVEL).
  std::vector<std::vector<u64>> branches;
  u64 root=0;
};
struct NativeCreationCatalogLayout {
  std::array<NativeCreationRelationLayout,6> relations;
  u64 image_count=0,root_cells=0,leaf_cells=0;
  // SB_TRANSACTION_HEADER_PAGE: the first transaction header page, the last
  // page allocated, so no catalog page number moves; 0 when none is planned.
  u64 transaction_header_page=0;
};
enum class NativeCreationLayoutError {none,invalid_input,capacity_exceeded,resource_exhausted};
struct NativeCreationLayoutResult {
  NativeCreationLayoutError error=NativeCreationLayoutError::invalid_input;
  std::optional<NativeCreationCatalogLayout> layout;
  bool ok() const noexcept{return error==NativeCreationLayoutError::none&&layout.has_value();}
};
// Shared exact physical layout for construction and original-operation recovery.
// transaction_header_page: the create writes the in-place inventory, so its
// header page is planned (SB_TRANSITIONAL_LEGACY_INVENTORY; no default, so every
// caller plans the format it writes).
// The fixed-root graph has 13 control pages (no checkpoint, horizon-root,
// selector or watermark page; first catalog leaf at control 8); the drift-era
// graph's layout went with the checkpoint layer (W4d).
// No identities, authorization, memory grants, writes or configuration defaults.
inline NativeCreationLayoutResult PlanNativeCreationCatalogLayout(
    u64 size,u64 maps,u64 total,u64 budget,const NativeCreationCatalogSeed& seed,
    const NativeCreationCatalogIndexes& indexes,bool transaction_header_page) noexcept {
  using E=NativeCreationLayoutError;
  const auto fail=[](E e){return NativeCreationLayoutResult{e,{}};};
  const u64 controls=14,first_leaf=8;
  try {
    if(!std::any_of(disk::kCanonicalFilespacePageProfiles.begin(),disk::kCanonicalFilespacePageProfiles.end(),
        [size](const auto& p){return p.page_size_bytes==size;})||
        maps>std::numeric_limits<u64>::max()-20||total<maps+controls)
      return fail(E::invalid_input);
    if(budget/size<2||maps+controls>budget/size-2)return fail(E::resource_exhausted);
    NativeCreationCatalogLayout plan;plan.image_count=maps+controls;
    plan.root_cells=NativeCatalogBtreeRootCells(size);plan.leaf_cells=NativeCatalogBtreeLeafCells(size);
    const u64 capacity=size-128-32-page::kRowDataPageBodyHeaderBytes;
    for(unsigned role=0;role<seed.size();++role) {
      const auto& rows=seed[role];auto& p=plan.relations[role];
      const auto shape=NativeCatalogBtreeShapeFor(size,rows.size());
      if(indexes[role]?!shape.ok():rows.size()>size/128)
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
      p.bases.push_back(maps+first_leaf+role);
      for(std::size_t i=1;i<p.starts.size();++i)p.bases.push_back(allocate());
      if(indexes[role]) {
        // Root, then leaves, then branches level by level: a tree of depth
        // two or less keeps the page numbers it had before branches existed.
        p.root=allocate();
        for(u64 i=0;i<shape.leaves();++i)p.navigation.push_back(allocate());
        for(std::size_t level=1;level+1<shape.depth;++level) {
          auto& pages=p.branches.emplace_back();
          for(u64 i=0;i<shape.level_pages[level];++i)pages.push_back(allocate());
        }
      }
    }
    if(transaction_header_page){
      // The transaction header page (owner 2026-11-05, Q3 = 1), after every
      // catalog page. A transitional legacy inventory (SBTINV01) has none
      // (SB_TRANSITIONAL_LEGACY_INVENTORY).
      if(plan.image_count>=total)return fail(E::capacity_exceeded);
      if(plan.image_count>=budget/size-2)return fail(E::resource_exhausted);
      plan.transaction_header_page=plan.image_count++;
    }
    return {E::none,std::move(plan)};
  }catch(E e){return fail(e);}
   catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
}

// SB_CATALOG_BTREE_N_LEVEL. The one bottom-up construction of a relation's
// catalog B-tree, shared by native create and by create-recovery, which must
// rebuild the same bytes. `cells` are the leaf cells in key order; the pages
// come from `layout`. `node(n,type,level)` returns page n's skeleton (header,
// dependencies, creator); `emit(n,page)` takes each finished page and returns
// false to stop. Pages are emitted leaves first, level by level, root last.
//  - Leaves take `leaf_cells` cells each, in order; the last takes the rest.
//  - Each level's children are spread evenly over the pages above it, so no
//    branch is empty (a branch needs a first child and at least one cell).
//  - A separator, and a page's low fence, is the first key of its subtree; a
//    high fence is the first key of the next page on the same level.
//  - Sibling links run across each whole level, over parent boundaries.
//  - Branches are 0x201; the root is 0x200 at level depth-1.
// Returns false, without emitting a page, when the cells and layout disagree.
template<class Ref,class Node,class Emit>
bool BuildNativeCreationCatalogBtree(const NativeCreationRelationLayout& layout,u64 leaf_cells,
    std::vector<page::NativeBtreeCell> cells,Ref ref,Node node,Emit emit) {
  if(layout.navigation.empty()) {
    if(!layout.branches.empty())return false;
    auto root=node(layout.root,0x200,0);root.cells=std::move(cells);
    return emit(layout.root,root);
  }
  const auto& leaves=layout.navigation;
  if(!leaf_cells||cells.size()<=(leaves.size()-1)*leaf_cells||cells.size()>leaves.size()*leaf_cells)return false;
  const std::size_t depth=layout.branches.size()+2;
  const auto level_pages=[&](std::size_t level)->std::span<const u64>{
    if(level==0)return leaves;
    if(level+1<depth)return layout.branches[level-1];
    return {&layout.root,1};
  };
  // Child i of n children spread over m parents: the first n%m parents take
  // n/m+1 children, the rest n/m.
  const auto first_child=[](std::size_t n,std::size_t m,std::size_t j){return j*(n/m)+std::min(j,n%m);};
  const auto parent_of=[](std::size_t n,std::size_t m,std::size_t i){
    const auto q=n/m,r=n%m;return i<r*(q+1)?i/(q+1):r+(i-r*(q+1))/q;};
  for(std::size_t level=1;level<depth;++level) {
    const auto n=level_pages(level-1).size(),m=level_pages(level).size();
    if(!m||n<2*m)return false;
  }
  // First key of each page's subtree on the level being built.
  std::vector<const page::NativeBtreeKey*> firsts(leaves.size()),above;
  for(std::size_t i=0;i<leaves.size();++i)firsts[i]=&cells[i*leaf_cells].key;
  for(std::size_t level=0;level<depth;++level) {
    const auto pages=level_pages(level);
    const auto below=level?level_pages(level-1):std::span<const u64>{};
    const bool top=level+1==depth;
    const auto parents=top?std::span<const u64>{}:level_pages(level+1);
    if(level)above.resize(pages.size());
    for(std::size_t i=0;i<pages.size();++i) {
      auto p=node(pages[i],top?0x200:level?0x201:0x202,static_cast<u16>(level));
      if(level) {
        const auto begin=first_child(below.size(),pages.size(),i),end=first_child(below.size(),pages.size(),i+1);
        p.first_child=ref(below[begin]);
        for(auto c=begin+1;c<end;++c)p.cells.push_back({*firsts[c],false,ref(below[c]),{}});
        above[i]=firsts[begin];
      } else {
        const auto begin=i*leaf_cells,end=std::min<u64>(begin+leaf_cells,cells.size());
        p.cells.assign(cells.begin()+begin,cells.begin()+end);
      }
      if(!top) {
        const auto& own=level?above:firsts;
        p.parent=ref(parents[parent_of(pages.size(),parents.size(),i)]);
        if(i){p.left=ref(pages[i-1]);p.low_fence=*own[i];}
        if(i+1<pages.size()) {
          p.right=ref(pages[i+1]);
          p.high_fence=level?*firsts[first_child(below.size(),pages.size(),i+1)]:*firsts[i+1];
        }
      }
      if(!emit(pages[i],p))return false;
    }
    if(level)firsts.swap(above);
  }
  return true;
}
} // namespace scratchbird::storage::database::detail
