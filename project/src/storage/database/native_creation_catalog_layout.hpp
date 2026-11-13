// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_catalog_input.hpp"
#include "row_data_page.hpp"
#include "native_btree_tree_backing.hpp"
#include "catalog_name_path.hpp"
#include "catalog_name_tree_keys.hpp"
#include <map>
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
// SEARCH_KEY: SB_NATIVE_NAME_TREES. One name tree's pages and leaf packing:
// `tree` holds the pages as a relation layout does (root, navigation = leaves,
// branches); `leaf_starts[i]` is the index of the first cell on leaf i, by
// bytes, not by count, since name keys vary in length. Empty leaf_starts with
// no leaves: the root is the only page and holds every cell (none, for an
// empty names relation).
struct NativeCreationNameTreeLayout {
  NativeCreationRelationLayout tree;
  std::vector<std::size_t> leaf_starts;
  std::size_t cells=0;
};
// SB_NATIVE_COMMENT_RELATION: page zero and the maps aside, the fixed-root
// graph's control block: the inventory, directory, database state, retention,
// catalog, configuration and security roots (offsets 1..7), then the first
// leaf of each of roles 1..6 (offsets 8..13); 14 counts page zero's slot too,
// as the planner's `controls` always has. Role 7, the comments relation, has
// no fixed first leaf: it is empty at create and, indexed, has no base leaf
// at all (an allocated leaf nothing reaches would be an orphan to startup's
// allocation check); its first insert allocates its first leaf. Were a seed to
// give it rows, its leaves would be allocated after the fixed block.
inline constexpr u64 kNativeCreationFixedLeafRoles=6;
inline constexpr u64 kNativeCreationControlPages=8+kNativeCreationFixedLeafRoles;
inline constexpr u64 kNativeCreationFirstLeafControl=8;
// The seed position of the comments relation (role 7).
inline constexpr std::size_t kNativeCommentRelationIndex=page::kNativeCommentRelationRole-1;
struct NativeCreationCatalogLayout {
  std::array<NativeCreationRelationLayout,page::kNativeCatalogRelationRoles> relations;
  u64 image_count=0,root_cells=0,leaf_cells=0;
  // SB_TRANSACTION_HEADER_PAGE: the first transaction header page, allocated
  // after the catalog pages of roles 1..6, so none of their page numbers
  // moves; 0 when none is planned. Role 7's pages (SB_NATIVE_COMMENT_RELATION)
  // and the name trees (below) are allocated after it for the same reason.
  u64 transaction_header_page=0;
  // SB_NATIVE_NAME_TREES: the authoritative name tree (root kind 22) then the
  // UUID-to-name tree (kind 23); planned only when the caller asked for them.
  std::optional<std::array<NativeCreationNameTreeLayout,2>> name_trees;
  // SB_NATIVE_COMMENT_TREE (2026-11-13): the comments relation's (target,
  // language) tree (root kind 24), after the name trees; planned only when the
  // caller asked for it. The comments relation is empty at create, so the tree
  // is its root alone, with no cells.
  std::optional<u64> comment_tree_root;
};
// SB_NATIVE_NAME_TREES. One cell of a name tree before placement: the names
// relation row it indexes (an index into seed[1]), its key and its included
// payload (catalog_name_tree_keys). The create fills in the row's base page
// and version when the row is placed. Built by the planner (for sizes) and
// by the workspace (for pages) from the same seed, in the same order.
struct NativeCreationNameTreeCellSeed {
  std::size_t row=0;
  std::vector<byte> key,payload;
};
// The cells of both trees, each in key order (then row UUID: the version UUID,
// the last tie-break, is issued in row order and never needed to order cells
// of distinct rows). The authoritative tree: one cell per name entry. The
// UUID-to-name tree: one per vector, then one per entry with the display
// components of its path (SB_NATIVE_NAME_PATH). Nullopt when a name's object
// has no definition in the seed, or a path does not resolve.
inline std::optional<std::array<std::vector<NativeCreationNameTreeCellSeed>,2>> BuildNativeCreationNameTreeCellSeeds(
    const NativeCreationCatalogSeed& seed) {
  namespace c=core::catalog;
  std::map<Uuid,c::CatalogRecordKind> kinds;
  for(const auto& relation:seed)for(const auto& record:relation)
    if(!record.name)kinds[record.metadata.record.header.object_uuid.value]=record.metadata.record.header.kind;
  c::CatalogNameIndex names;
  for(const auto& record:seed[1])if(record.name)names.Add(c::BorrowCatalogNamePayload(*record.name));
  std::array<std::vector<NativeCreationNameTreeCellSeed>,2> out;
  for(std::size_t i=0;i<seed[1].size();++i){
    const auto& record=seed[1][i];if(!record.name)continue;
    if(const auto* vector=std::get_if<c::CatalogNameVector>(&*record.name)){
      const auto kind=kinds.find(vector->object_uuid.value);if(kind==kinds.end())return std::nullopt;
      const auto view=std::get<c::CatalogNameVectorView>(c::BorrowCatalogNamePayload(*record.name));
      out[1].push_back({i,c::CatalogNameUuidTreeVectorKey(vector->object_uuid.value,kind->second),
          c::CatalogNameUuidTreeVectorPayload(view)});
    }else{
      const auto& entry=std::get<c::CatalogNameEntry>(*record.name);
      const auto kind=kinds.find(entry.object_uuid.value);if(kind==kinds.end())return std::nullopt;
      const auto view=c::BorrowCatalogNameEntry(entry);
      out[0].push_back({i,c::CatalogNameAuthoritativeTreeKey(view,kind->second),c::CatalogNameAuthoritativeTreePayload(view)});
      const auto path=c::ResolveCatalogNamePath(view,names);
      if(!path.ok())return std::nullopt;
      std::vector<std::string_view> components;components.reserve(path.components.size());
      for(const auto& component:path.components)components.push_back(component.display_name);
      out[1].push_back({i,c::CatalogNameUuidTreeEntryKey(view,kind->second),
          c::CatalogNameUuidTreeEntryPayload(entry.name_entry_uuid.value,components)});
    }
  }
  for(auto& cells:out)std::sort(cells.begin(),cells.end(),[&](const auto& a,const auto& b){
    if(std::lexicographical_compare(a.key.begin(),a.key.end(),b.key.begin(),b.key.end()))return true;
    if(std::lexicographical_compare(b.key.begin(),b.key.end(),a.key.begin(),a.key.end()))return false;
    return seed[1][a.row].metadata.record.header.row_uuid.value<seed[1][b.row].metadata.record.header.row_uuid.value;});
  return out;
}
// SB_NATIVE_NAME_TREES: how full the create packs a name tree leaf. Three
// quarters, so the first named inserts after the create (N3) fit their leaves
// before page splits exist (W3b-3) and a rename can stamp an old version's
// cell in place. A measured choice is for the page manager rebuild, which will
// have the tests to pick one; this constant and the estimate below are what to
// revisit then.
inline constexpr std::size_t kNativeNameTreeCreateFillNumerator=3,kNativeNameTreeCreateFillDenominator=4;
// Upper bounds on one SBBTP002 cell's encoded size with no prefix compression
// credited: flags and the two counts, the whole key, the version UUID, a
// creator of up to 10 LEB128 bytes, the compact base reference, the payload
// length and bytes, and a share of the jump table (one byte, rounded up).
inline constexpr std::size_t NativeNameTreeLeafCellBytes(std::size_t key,std::size_t payload) noexcept {
  return 3+key+16+10+16+10+payload+1;
}
inline constexpr std::size_t NativeNameTreeBranchCellBytes(std::size_t key) noexcept {return 3+key+16+1;}

enum class NativeCreationLayoutError {none,invalid_input,capacity_exceeded,resource_exhausted};
struct NativeCreationLayoutResult {
  NativeCreationLayoutError error=NativeCreationLayoutError::invalid_input;
  std::optional<NativeCreationCatalogLayout> layout;
  bool ok() const noexcept{return error==NativeCreationLayoutError::none&&layout.has_value();}
};
// Shared exact physical layout for construction and original-operation recovery.
// The create writes the in-place inventory, so its transaction header page is
// always planned (W4d-3: the legacy SBTINV01 inventory, which had none, and
// the flag that chose between them were removed).
// The fixed-root graph has 13 control pages (no checkpoint, horizon-root,
// selector or watermark page; first catalog leaf at control 8, roles 1..6;
// SB_NATIVE_COMMENT_RELATION: role 7 has none, see above); the drift-era
// graph's layout went with the checkpoint layer (W4d).
// No identities, authorization, memory grants, writes or configuration defaults.
inline NativeCreationLayoutResult PlanNativeCreationCatalogLayout(
    u64 size,u64 maps,u64 total,u64 budget,const NativeCreationCatalogSeed& seed,
    const NativeCreationCatalogIndexes& indexes,bool name_trees=false,bool comment_tree=false) noexcept {
  using E=NativeCreationLayoutError;
  const auto fail=[](E e){return NativeCreationLayoutResult{e,{}};};
  const u64 controls=kNativeCreationControlPages,first_leaf=kNativeCreationFirstLeafControl;
  try {
    if(!std::any_of(disk::kCanonicalFilespacePageProfiles.begin(),disk::kCanonicalFilespacePageProfiles.end(),
        [size](const auto& p){return p.page_size_bytes==size;})||
        maps>std::numeric_limits<u64>::max()-20||total<maps+controls)
      return fail(E::invalid_input);
    if(budget/size<2||maps+controls>budget/size-2)return fail(E::resource_exhausted);
    NativeCreationCatalogLayout plan;plan.image_count=maps+controls;
    plan.root_cells=NativeCatalogBtreeRootCells(size);plan.leaf_cells=NativeCatalogBtreeLeafCells(size);
    const u64 capacity=size-128-32-page::kRowDataPageBodyHeaderBytes;
    // Roles 1..6, the transaction header page, then role 7
    // (SB_NATIVE_COMMENT_RELATION): the comments relation's pages come after
    // the header page, so no page number of the six-role layout moves.
    for(unsigned step=0;step<=seed.size();++step) {
      if(step==kNativeCreationFixedLeafRoles) {
        // The transaction header page (owner 2026-11-05, Q3 = 1), after every
        // catalog page of roles 1..6.
        if(plan.image_count>=total)return fail(E::capacity_exceeded);
        if(plan.image_count>=budget/size-2)return fail(E::resource_exhausted);
        plan.transaction_header_page=plan.image_count++;
        continue;
      }
      const unsigned role=step<kNativeCreationFixedLeafRoles?step:step-1;
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
      if(role<kNativeCreationFixedLeafRoles)p.bases.push_back(maps+first_leaf+role);
      else if(!indexes[role]||!rows.empty())p.bases.push_back(allocate());
      else p.starts.clear();  // indexed and empty: no base leaf (SB_NATIVE_COMMENT_RELATION)
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
    if(name_trees){
      // SB_NATIVE_NAME_TREES: both trees, after the header page. Leaves are
      // packed by estimated bytes to the create fill; every level above is
      // sized from the longest key, which bounds a separator.
      auto cells=BuildNativeCreationNameTreeCellSeeds(seed);
      if(!cells)return fail(E::invalid_input);
      const auto allocate=[&]() {
        if(plan.image_count>=total)throw E::capacity_exceeded;
        if(plan.image_count>=budget/size-2)throw E::resource_exhausted;
        return plan.image_count++;
      };
      std::array<NativeCreationNameTreeLayout,2> trees;
      for(unsigned t=0;t<2;++t){
        auto& layout=trees[t];const auto& tree_cells=(*cells)[t];layout.cells=tree_cells.size();
        std::size_t longest_key=0,largest_cell=0,total_bytes=0;
        for(const auto& cell:tree_cells){
          if(cell.key.size()+cell.payload.size()>size/4)return fail(E::capacity_exceeded);  // key_too_long
          longest_key=std::max(longest_key,cell.key.size());
          const auto bytes=NativeNameTreeLeafCellBytes(cell.key.size(),cell.payload.size());
          largest_cell=std::max(largest_cell,bytes);total_bytes+=bytes;
        }
        // Prefix 640; a leaf or branch carries two fences of up to the longest key.
        const std::size_t fences=2*(longest_key+40);
        if(size<640+fences+2*largest_cell)return fail(E::capacity_exceeded);
        const std::size_t root_budget=(size-640)*kNativeNameTreeCreateFillNumerator/kNativeNameTreeCreateFillDenominator;
        const std::size_t leaf_budget=std::max(largest_cell,(size-640-fences)*kNativeNameTreeCreateFillNumerator/kNativeNameTreeCreateFillDenominator);
        layout.tree.root=allocate();
        if(total_bytes<=root_budget)continue;  // the root is the only page
        std::size_t used=0;
        for(std::size_t i=0;i<tree_cells.size();++i){
          const auto bytes=NativeNameTreeLeafCellBytes(tree_cells[i].key.size(),tree_cells[i].payload.size());
          if(layout.leaf_starts.empty()||used+bytes>leaf_budget){layout.leaf_starts.push_back(i);used=0;}
          used+=bytes;
        }
        if(layout.leaf_starts.size()==1)layout.leaf_starts.push_back(tree_cells.size()/2);  // a branch needs two children
        for(std::size_t i=0;i<layout.leaf_starts.size();++i)layout.tree.navigation.push_back(allocate());
        const std::size_t branch_cell=NativeNameTreeBranchCellBytes(longest_key);
        const std::size_t fanout=std::max<std::size_t>(2,(size-640-fences)*kNativeNameTreeCreateFillNumerator/kNativeNameTreeCreateFillDenominator/branch_cell);
        const std::size_t root_fanout=std::max<std::size_t>(2,root_budget/branch_cell);
        std::size_t children=layout.tree.navigation.size();
        while(children>root_fanout){
          std::size_t pages=(children+fanout-1)/fanout;
          if(children<2*pages)pages=children/2;
          auto& level=layout.tree.branches.emplace_back();
          for(std::size_t i=0;i<pages;++i)level.push_back(allocate());
          children=pages;
        }
      }
      plan.name_trees=std::move(trees);
    }
    if(comment_tree){
      // SB_NATIVE_COMMENT_TREE: one root page; a create with comment rows has
      // no tree layout (the native create writes none, owner 2026-11-13).
      if(!seed[kNativeCommentRelationIndex].empty())return fail(E::invalid_input);
      if(plan.image_count>=total)return fail(E::capacity_exceeded);
      if(plan.image_count>=budget/size-2)return fail(E::resource_exhausted);
      plan.comment_tree_root=plan.image_count++;
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
//  - With `leaf_starts` (SB_NATIVE_NAME_TREES), leaf i holds the cells from
//    leaf_starts[i] to the next start instead: one start per leaf, the first 0,
//    strictly increasing, all inside `cells`.
template<class Ref,class Node,class Emit>
bool BuildNativeCreationCatalogBtree(const NativeCreationRelationLayout& layout,u64 leaf_cells,
    std::vector<page::NativeBtreeCell> cells,Ref ref,Node node,Emit emit,
    std::span<const std::size_t> leaf_starts={}) {
  if(layout.navigation.empty()) {
    if(!layout.branches.empty()||!leaf_starts.empty())return false;
    auto root=node(layout.root,0x200,0);root.cells=std::move(cells);
    return emit(layout.root,root);
  }
  const auto& leaves=layout.navigation;
  if(leaf_starts.empty()){
    if(!leaf_cells||cells.size()<=(leaves.size()-1)*leaf_cells||cells.size()>leaves.size()*leaf_cells)return false;
  }else{
    if(leaf_starts.size()!=leaves.size()||leaf_starts.front()!=0||leaf_starts.back()>=cells.size())return false;
    for(std::size_t i=1;i<leaf_starts.size();++i)if(leaf_starts[i]<=leaf_starts[i-1])return false;
  }
  const auto leaf_begin=[&](std::size_t i){return leaf_starts.empty()?i*leaf_cells:leaf_starts[i];};
  const auto leaf_end=[&](std::size_t i){return i+1<leaves.size()?leaf_begin(i+1):cells.size();};
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
  for(std::size_t i=0;i<leaves.size();++i)firsts[i]=&cells[leaf_begin(i)].key;
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
        const auto begin=leaf_begin(i),end=leaf_end(i);
        p.cells.assign(cells.begin()+static_cast<std::ptrdiff_t>(begin),cells.begin()+static_cast<std::ptrdiff_t>(end));
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
