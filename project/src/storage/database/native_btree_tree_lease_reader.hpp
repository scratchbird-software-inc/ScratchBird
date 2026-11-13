// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_btree_tree_backing.hpp"
#include "native_lease_read_source.hpp"

namespace scratchbird::storage::database {
// Navigation only: the caller's root/dependency tuple is not catalog authority.
// Admit backing before the source handle; release it after the handle. Reuse
// invalidates all previous borrowed results even when traversal fails.
class NativeCatalogRelationLeaseReader;
class NativeBtreeTreeLeaseReader {
 public:
  // SB_NATIVE_FIXED_ROOT_READ: the same read from page zero's fixed roots.
  static NativeBtreeTreeReadResult Read(const NativeFixedRootMemoryLease& lease,
      const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
      NativeBtreeTreePreparedMemory& backing,
      std::size_t image_limit=std::numeric_limits<std::size_t>::max()) noexcept {
    return ReadWith(detail::NativeLeaseSourceAccess::From(lease),root,dependencies,backing,image_limit);
  }
 private:
  friend class NativeCatalogRelationLeaseReader;
  static NativeBtreeTreeReadResult ReadWith(const detail::NativeLeaseReadSource& source,
      const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
      NativeBtreeTreePreparedMemory& backing,std::size_t image_limit,const Uuid* selected_index=nullptr) noexcept {
    if(source.database_uuid.is_nil())return {};
    if(source.cluster_authority_required){
      NativeBtreeTreeReadResult out;out.error=NativeBtreeTreeMemoryError::tree_failure;
      out.tree_error=page::NativeBtreeError::cluster_requires_authority;return out;
    }
    return btree_memory_detail::Reader::Run(source.database_uuid,source.devices,
      {source.batches,source.batch_count},root,dependencies,backing,image_limit,selected_index);
  }
 public:
  // SB_NATIVE_NAME_TREES: a name tree from page zero's root set (kinds 22 and
  // 23). The root's object is the tree's index; its dependency tuple is adopted
  // from the tree's own pages, as a selected relation head's is, and the engine
  // compares it with the catalog's profile and map rows.
  static NativeBtreeTreeReadResult ReadPageZeroTree(const detail::NativeLeaseReadSource& source,
      const detail::NativeLeaseRoot& root,NativeBtreeTreePreparedMemory& backing,std::size_t image_limit) noexcept {
    if(root.kind!=22&&root.kind!=23&&root.kind!=24)return {};  // 24: SB_NATIVE_COMMENT_TREE
    return ReadWith(source,root.page,{},backing,image_limit,&root.object_uuid);
  }
 private:
  // This entry is restricted to traversal of a head obtained from the actual
  // selected catalog root. It is not a public arbitrary-page discovery API.
  static NativeBtreeTreeReadResult ReadSelected(const detail::NativeLeaseReadSource& source,
      const disk::NativePageReference& root,const Uuid& selected_index,
      NativeBtreeTreePreparedMemory& backing,std::size_t image_limit) noexcept {
    return ReadWith(source,root,{},backing,image_limit,&selected_index);
  }
};
} // namespace scratchbird::storage::database
