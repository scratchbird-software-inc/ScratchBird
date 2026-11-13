// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_lease_read_source.hpp"
#include "catalog_comment_record_codec.hpp"
#include <concepts>

namespace scratchbird::storage::database {
enum class NativeCatalogCohortError {
  none,invalid_request,root_failure,relation_failure,unresolved_creator,
  binding_mismatch,resource_exhausted,
  // SB_NATIVE_NAME_TREES: one name tree root without the other, a root of the
  // wrong page type or object, or a tree that does not read; a cell that does
  // not bind to a names-relation row version (or a row version without its
  // cells).
  name_tree_failure,name_tree_mismatch,
  // SB_NATIVE_COMMENT_TREE: the comment tree root (kind 24) of the wrong page
  // type or object, or a tree that does not read; a cell that does not bind to
  // a comment row version of the comments relation, or whose key is not that
  // row's target and language (or a comment row version without its cell).
  comment_tree_failure,comment_tree_mismatch,
  // SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1, 2026-11-13): a name or
  // comment row with two versions by one transaction (a transaction's
  // successor of its own version replaces it in place, so no writer leaves
  // two).
  own_version_repeated
};
struct NativeCatalogRelationInspection {
  u16 selector=0,role=0;
  Uuid catalog_uuid,relation_uuid;
  disk::NativePageReference root;
  u64 catalog_generation=0,schema_epoch=0,security_epoch=0,resource_epoch=0;
  usize base_pages=0,navigation_pages=0,versions=0,visible_rows=0;
  usize bindings=0,maximum_page_bytes=0;
  u64 navigation_image_bytes=0;
};
struct NativeCatalogCohortInspection {
  NativeCatalogCohortError error=NativeCatalogCohortError::invalid_request;
  NativeCatalogRootsLeaseError root_error=NativeCatalogRootsLeaseError::none;
  NativePinnedCatalogReadError visibility_error=NativePinnedCatalogReadError::none;
  NativeCatalogRelationLeaseError relation_error=NativeCatalogRelationLeaseError::none;
  std::optional<NativeReadSourceIdentity> source;
  std::array<NativeCatalogRelationInspection,2*page::kNativeCatalogRelationRoles> relations{};
  usize relation_count=0;
  // SB_NATIVE_NAME_TREES: whether page zero had the name trees and what they held.
  bool name_trees_present=false;
  std::array<usize,2> name_tree_pages{},name_tree_cells{};
  // SB_NATIVE_COMMENT_TREE: whether page zero had the comment tree and what it held.
  bool comment_tree_present=false;
  usize comment_tree_pages=0,comment_tree_cells=0;
  NativeBtreeTreeMemoryError name_tree_error=NativeBtreeTreeMemoryError::none;
  page::NativeBtreeError name_tree_page_error=page::NativeBtreeError::none;
  u16 failing_selector=0,failing_role=0;
  Uuid unresolved_transaction_uuid;
  u64 unresolved_local_transaction_id=0,physical_bytes_read=0,verified_image_bytes=0;
  bool ok() const noexcept{return error==NativeCatalogCohortError::none&&source.has_value();}
};
struct NativeCatalogCohortPreparedMemory {
  NativeCatalogRootsPreparedMemory roots;
  NativeCatalogVisibilityPreparedMemory visibility;
  // SB_NATIVE_NAME_TREES: one tree at a time; empty when the limits asked for none.
  NativeBtreeTreePreparedMemory name_trees;
  explicit operator bool() const noexcept{return bool(roots)&&bool(visibility);}
};
struct NativeCatalogCohortPreparedResult {
  NativeCatalogRelationLeaseError error=NativeCatalogRelationLeaseError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeCatalogCohortPreparedMemory workspace;
  bool ok() const noexcept{return error==NativeCatalogRelationLeaseError::none&&bool(workspace);}
};
inline usize NativeCatalogCohortWorkspaceBytes(const NativeCatalogRelationMemoryLimits& limits) noexcept {
  const auto roots=NativeCatalogRootsWorkspaceBytes(limits.maximum_profile);
  const auto visibility=NativeCatalogVisibilityWorkspaceBytes(limits);
  if(!roots||!visibility||roots>SIZE_MAX-visibility)return 0;
  const auto base=roots+visibility;
  if(!limits.name_trees.maximum_pages)return base;
  const auto trees=NativeBtreeTreeWorkspaceBytes(limits.maximum_profile,limits.maximum_devices,limits.name_trees);
  return trees&&trees<=SIZE_MAX-base?base+trees:0;
}
// One reusable, admitted relation workspace covers every declared role. It is
// prepared before source guards and reused only after each role's complete
// observation has been consumed. No row text or borrowed mutable view escapes.
inline NativeCatalogCohortPreparedResult PrepareNativeCatalogCohortRead(
    const NativeCatalogRelationMemoryLimits& limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  NativeCatalogCohortPreparedResult out;
  if(!NativeCatalogCohortWorkspaceBytes(limits))return out;
  auto root=PrepareNativeCatalogRootsRead(limits.maximum_profile,memory,binding);
  out.memory_error=root.memory_error;out.allocation_status=root.allocation_status;
  out.allocation_diagnostic=std::move(root.allocation_diagnostic);
  if(!root.ok()){out.error=NativeCatalogRelationLeaseError::memory_failure;return out;}
  auto visibility=PrepareNativeCatalogVisibilityRead(limits,memory,binding);
  out.memory_error=visibility.memory_error;out.allocation_status=visibility.allocation_status;
  out.allocation_diagnostic=std::move(visibility.allocation_diagnostic);
  if(!visibility.ok()){out.error=visibility.error;return out;}
  out.workspace.roots=std::move(root.workspace);out.workspace.visibility=std::move(visibility.workspace);
  if(limits.name_trees.maximum_pages){
    auto trees=PrepareNativeBtreeTreeRead(limits.maximum_profile,limits.maximum_devices,limits.name_trees,memory,binding);
    out.memory_error=trees.memory_error;out.allocation_status=trees.allocation_status;
    out.allocation_diagnostic=std::move(trees.allocation_diagnostic);
    if(!trees.ok()){out.error=NativeCatalogRelationLeaseError::memory_failure;return out;}
    out.workspace.name_trees=std::move(trees.workspace);
  }
  out.error=NativeCatalogRelationLeaseError::none;return out;
}
// Actual selected catalog/navigation/base-leaf coverage, metadata chains and
// committed row visibility. This is a verifier input, not a complete engine
// verification receipt: cross-family semantic references, user relations,
// constraints, recovery decisions and service admission remain engine-owned.
class NativeCatalogCohortLeaseReader {
 public:
  // SB_NATIVE_FIXED_ROOT_READ: through page zero's fixed roots (the selected-
  // checkpoint lease went with that layer, W4d). Database, inventory,
  // allocation and directory come from the lease-neutral view.
  template<class Lease>
    requires std::same_as<Lease,NativeFixedRootMemoryLease>
  static NativeCatalogCohortInspection Read(const Lease& source,
      u64 maximum_image_bytes,NativeCatalogCohortPreparedMemory& workspace,NativeCatalogCapture* capture=nullptr) noexcept {
    using E=NativeCatalogCohortError;NativeCatalogCohortInspection out;
    constexpr bool fixed_root=std::same_as<Lease,NativeFixedRootMemoryLease>;
    const auto view=detail::NativeLeaseSourceAccess::From(source);
    if(capture)capture->Begin();
    const auto fail=[&](E error){out.error=error;out.source.reset();out.relations={};out.relation_count=0;
      if(capture)capture->Begin();return out;};
    if(!workspace||source.retained_image_bytes()>=maximum_image_bytes)return fail(E::invalid_request);
    if(capture&&(!*capture||!view.inventory||capture->arena_.binding().database_uuid!=view.database_uuid))
      return fail(E::invalid_request);
    const auto roots=NativeCatalogRootsLeaseReader::Read(source,maximum_image_bytes-source.retained_image_bytes(),workspace.roots);
    out.root_error=roots.error;out.physical_bytes_read=roots.physical_bytes_read;
    if(!roots.ok())return fail(E::root_failure);
    out.verified_image_bytes=source.retained_image_bytes()+roots.retained_image_bytes;
    if(!view.inventory||!view.directory||!view.allocation)return fail(E::invalid_request);
    const auto& inventory=*view.inventory;
    if(capture&&!capture->transactions_.empty()){
      if(inventory.entries.size()>capture->transactions_.size())return fail(E::resource_exhausted);
      std::copy(inventory.entries.begin(),inventory.entries.end(),capture->transactions_.begin());
      capture->inventory_=NativeCapturedTransactionInventory{
          view.inventory_generation,
          {inventory.next_local_transaction_id,inventory.next_commit_sequence,
           capture->transactions_.first(inventory.entries.size())}};
    }
    for(usize catalog_index=0;catalog_index<roots.catalogs.size();++catalog_index){
      const u16 selector=catalog_index==roots.feature_root_index&&catalog_index!=0?8:2;
      const auto& root=roots.catalogs[catalog_index].root;
      if(capture&&!capture->AppendPhysical(root.header,root.object_uuid))return fail(E::resource_exhausted);
      for(const auto& relation:root.roots){
        out.failing_selector=selector;out.failing_role=relation.role;
        if(out.relation_count==out.relations.size()||out.verified_image_bytes>=maximum_image_bytes)
          return fail(E::resource_exhausted);
        const auto allowance=maximum_image_bytes-out.verified_image_bytes+source.retained_image_bytes();
        const auto read=NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(source,selector,relation.role,
          allowance,workspace.visibility);
        out.physical_bytes_read+=read.source.physical_bytes_read;
        out.visibility_error=read.error;out.relation_error=read.source.error;
        if(!read.ok())return fail(E::relation_failure);
        if(read.source.retained_image_bytes<source.retained_image_bytes())return fail(E::binding_mismatch);
        const auto additional=read.source.retained_image_bytes-source.retained_image_bytes();
        if(additional>maximum_image_bytes-out.verified_image_bytes)return fail(E::resource_exhausted);
        out.verified_image_bytes+=additional;
        for(const auto& creator:read.source.row_creators){
          if(creator.inventory_entry_index>=inventory.entries.size())return fail(E::binding_mismatch);
          const auto& entry=inventory.entries[creator.inventory_entry_index];
          const auto state=transaction::mga::InventoryVisibilityState(entry);
          // SB_CATALOG_VERSION_WRITER (in-place writes W3a, owner Q23 = 1):
          // a catalog version written in place by a transaction still marked
          // in progress (a server that stopped before COMMIT) is uncommitted,
          // hidden by the visibility read above, and its transaction is dead
          // once a writer restarts. The startup scan admits it. In-doubt and
          // transient creators (prepared, limbo, committing, rolling back,
          // recovering, created) stay unresolved.
          if(state!=transaction::mga::TransactionState::committed&&state!=transaction::mga::TransactionState::rolled_back&&
              state!=transaction::mga::TransactionState::failed_terminal&&state!=transaction::mga::TransactionState::active&&
              state!=transaction::mga::TransactionState::read_only_active){
            out.unresolved_transaction_uuid=entry.identity.transaction_uuid.value;
            out.unresolved_local_transaction_id=entry.identity.local_id.value;return fail(E::unresolved_creator);}
        }
        auto& inspection=out.relations[out.relation_count++];
        inspection={selector,relation.role,root.object_uuid,read.source.relation.relation_uuid,
          relation.page,root.catalog_generation,root.schema_epoch,root.security_epoch,root.resource_epoch,
          read.source.bases.size(),read.source.tree.pages.size(),read.source.row_creators.size(),read.rows.size()};
        inspection.bindings=read.source.bindings.size();inspection.navigation_image_bytes=read.source.tree.retained_image_bytes;
        if(capture){
          for(const auto& leaf:read.source.bases)
            if(!capture->AppendPhysical(leaf.page.header,read.source.relation.relation_uuid))return fail(E::resource_exhausted);
          for(const auto& page:read.source.tree.pages)
            if(!capture->AppendPhysical(page.page->header,page.page->dependencies.index_uuid))return fail(E::resource_exhausted);
        }
        inspection.maximum_page_bytes=root.header.page_size_bytes;
        for(const auto& leaf:read.source.bases)inspection.maximum_page_bytes=std::max<usize>(inspection.maximum_page_bytes,leaf.page.header.page_size_bytes);
        for(const auto& page:read.source.tree.pages)inspection.maximum_page_bytes=std::max<usize>(inspection.maximum_page_bytes,page.page->header.page_size_bytes);
        if(capture)for(const auto& row:read.rows)
          if(!capture->Append(selector,relation.role,row))return fail(E::resource_exhausted);
        if(capture&&!capture->AppendRelation({selector,relation.role,read.source.relation.relation_uuid,
            read.source.relation.index_dependencies}))return fail(E::resource_exhausted);
        if(selector==2&&(relation.role==2||relation.role==page::kNativeCommentRelationRole)){
          // SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1): at most one
          // version per (row, creator transaction) in the names and comments
          // relations, whatever the creator's state. Rows x rows over the
          // relation's base leaves (heap-free; a few milliseconds on the
          // default pack).
          const auto& bases=read.source.bases;
          for(usize a=0;a<bases.size();++a)for(usize i=0;i<bases[a].page.body.rows.size();++i){
            const auto& x=bases[a].page.body.rows[i];
            for(usize b=a;b<bases.size();++b)for(usize j=(b==a?i+1:0);j<bases[b].page.body.rows.size();++j){
              const auto& y=bases[b].page.body.rows[j];
              if(x.row_uuid.value==y.row_uuid.value&&x.local_transaction_id==y.local_transaction_id)
                return fail(E::own_version_repeated);
            }}
        }
        if(selector==2&&relation.role==2){
          // SB_NATIVE_NAME_TREES: the two name trees over this relation, read
          // from page zero (kinds 22 and 23) while the relation's base leaves
          // are still live, so every cell can be bound to its row version.
          const auto* first=view.Root(22);const auto* second=view.Root(23);
          if((first!=nullptr)!=(second!=nullptr))return fail(E::name_tree_failure);
          if(first){
            if(!workspace.name_trees)return fail(E::resource_exhausted);
            out.name_trees_present=true;
            const auto& bases=read.source.bases;
            for(unsigned t=0;t<2;++t){
              const auto& tree_root=t?*second:*first;
              if(tree_root.page_type!=0x200)return fail(E::name_tree_failure);
              if(out.verified_image_bytes>=maximum_image_bytes)return fail(E::resource_exhausted);
              const auto limit=static_cast<usize>(std::min<u64>(maximum_image_bytes-out.verified_image_bytes,std::numeric_limits<usize>::max()));
              const auto tree=NativeBtreeTreeLeaseReader::ReadPageZeroTree(view,tree_root,workspace.name_trees,limit);
              out.physical_bytes_read+=tree.physical_bytes_read;
              if(!tree.ok()){out.name_tree_error=tree.error;out.name_tree_page_error=tree.tree_error;return fail(E::name_tree_failure);}
              if(tree.retained_image_bytes>maximum_image_bytes-out.verified_image_bytes)return fail(E::resource_exhausted);
              out.verified_image_bytes+=tree.retained_image_bytes;
              const auto& dependencies=tree.pages.front().page->dependencies;
              if(dependencies.index_uuid!=tree_root.object_uuid)return fail(E::name_tree_failure);
              // Every cell binds to a row version of this relation: its base
              // leaf, that version in it, the row's identity and creator; the
              // row is a name row (an entry, for the authoritative tree).
              usize cells=0;
              for(const auto n:tree.leaves){const auto& page=*tree.pages[n].page;
                for(const auto& cell:page.cells){
                  ++cells;
                  if(!cell.base_page)return fail(E::name_tree_mismatch);
                  const NativeReadCatalogLeafImage* base=nullptr;
                  for(const auto& candidate:bases){const auto& h=candidate.page.header;
                    if(h.filespace_uuid==cell.base_page->filespace_uuid&&h.page_number==cell.base_page->page_number){
                      if(h.page_generation!=cell.base_page->page_generation||h.page_size_profile_uuid!=cell.base_page->page_size_profile_uuid)
                        return fail(E::name_tree_mismatch);
                      base=&candidate;break;}}
                  if(!base)return fail(E::name_tree_mismatch);
                  const auto& metadata=base->page.metadata;
                  const auto found=std::lower_bound(metadata.begin(),metadata.end(),cell.key.version_uuid,
                      [](const auto& m,const Uuid& id){return m.version_uuid<id;});
                  if(found==metadata.end()||found->version_uuid!=cell.key.version_uuid||!found->name)return fail(E::name_tree_mismatch);
                  const auto& row=base->page.body.rows[found->row_index];
                  if(row.row_uuid.value!=cell.key.row_uuid||row.local_transaction_id!=cell.creator_local_transaction_id)
                    return fail(E::name_tree_mismatch);
                  if(t==0&&!std::holds_alternative<core::catalog::CatalogNameEntryView>(*found->name))return fail(E::name_tree_mismatch);
                  if(capture&&!capture->AppendNameTreeCell(static_cast<u16>(t),cell))return fail(E::resource_exhausted);
                }}
              // Every row version has its cells: one in the authoritative tree
              // per entry, one in the UUID-to-name tree per name row (a deleted
              // version none). Counted
              // by scanning the tree for each row (rows x cells, a few
              // milliseconds on the default pack); with the cell count above
              // this is a bijection. The page manager rebuild can index it.
              usize expected=0;
              for(usize bi=0;bi<bases.size();++bi)for(const auto& m:bases[bi].page.metadata){
                if(!m.name)continue;
                // A drop's version (deleted) has no cells of its own: the
                // version it retired keeps its stamped ones (owner Q42, N4a).
                const bool wants=!m.metadata.record.header.deleted&&
                    (t==1||std::holds_alternative<core::catalog::CatalogNameEntryView>(*m.name));
                usize matches=0;
                for(const auto n:tree.leaves)for(const auto& cell:tree.pages[n].page->cells)
                  if(cell.key.version_uuid==m.version_uuid)++matches;
                if(matches!=(wants?1u:0u)){
                  // SB_NATIVE_NAME_CASCADE (N4b): a writer puts its base leaves
                  // before its tree pages, so a write that stopped between them
                  // leaves versions without their cells. Their creator never
                  // committed: in progress when the server stopped (dead once a
                  // writer restarts, owner Q23 = 1), rolled back or failed. Such a
                  // version may lack its cell (garbage a sweep removes); any other
                  // creator's may not.
                  bool admitted=false;
                  if(wants&&matches==0)
                    for(const auto& creator:read.source.row_creators){
                      if(creator.catalog_page_index!=bi||creator.catalog_row_index!=m.row_index)continue;
                      if(creator.inventory_entry_index>=inventory.entries.size())break;
                      const auto state=transaction::mga::InventoryVisibilityState(inventory.entries[creator.inventory_entry_index]);
                      admitted=state==transaction::mga::TransactionState::active||
                          state==transaction::mga::TransactionState::rolled_back||
                          state==transaction::mga::TransactionState::failed_terminal;
                      break;
                    }
                  if(!admitted)return fail(E::name_tree_mismatch);
                }
                expected+=matches;
              }
              if(cells!=expected)return fail(E::name_tree_mismatch);
              out.name_tree_cells[t]=cells;out.name_tree_pages[t]=tree.pages.size();
              if(capture){
                for(const auto& page:tree.pages)
                  if(!capture->AppendPhysical(page.page->header,dependencies.index_uuid))return fail(E::resource_exhausted);
                capture->name_trees_[t]=NativeCapturedNameTree{tree_root.kind,dependencies,tree_root.page,cells,tree.pages.size()};
              }
            }
          }
        }
        if(selector==2&&relation.role==page::kNativeCommentRelationRole){
          // SEARCH_KEY: SB_NATIVE_COMMENT_TREE (2026-11-13). The comments
          // relation's (target, language) tree, read from page zero (kind 24)
          // while the relation's base leaves are still live: every cell binds
          // to a comment row version of this relation, its key is that row's
          // target16 | language16, and every comment row version that is not
          // a drop has exactly one cell (the name trees' rule, with the same
          // admission for an uncommitted writer's version that lost its cell).
          if(const auto* comment_root=view.Root(24)){
            if(!workspace.name_trees)return fail(E::resource_exhausted);
            out.comment_tree_present=true;
            const auto& bases=read.source.bases;
            if(comment_root->page_type!=0x200)return fail(E::comment_tree_failure);
            if(out.verified_image_bytes>=maximum_image_bytes)return fail(E::resource_exhausted);
            const auto limit=static_cast<usize>(std::min<u64>(maximum_image_bytes-out.verified_image_bytes,std::numeric_limits<usize>::max()));
            const auto tree=NativeBtreeTreeLeaseReader::ReadPageZeroTree(view,*comment_root,workspace.name_trees,limit);
            out.physical_bytes_read+=tree.physical_bytes_read;
            if(!tree.ok()){out.name_tree_error=tree.error;out.name_tree_page_error=tree.tree_error;return fail(E::comment_tree_failure);}
            if(tree.retained_image_bytes>maximum_image_bytes-out.verified_image_bytes)return fail(E::resource_exhausted);
            out.verified_image_bytes+=tree.retained_image_bytes;
            const auto& dependencies=tree.pages.front().page->dependencies;
            if(dependencies.index_uuid!=comment_root->object_uuid)return fail(E::comment_tree_failure);
            usize cells=0;
            for(const auto n:tree.leaves){const auto& page=*tree.pages[n].page;
              for(const auto& cell:page.cells){
                ++cells;
                if(!cell.base_page)return fail(E::comment_tree_mismatch);
                const NativeReadCatalogLeafImage* base=nullptr;
                for(const auto& candidate:bases){const auto& h=candidate.page.header;
                  if(h.filespace_uuid==cell.base_page->filespace_uuid&&h.page_number==cell.base_page->page_number){
                    if(h.page_generation!=cell.base_page->page_generation||h.page_size_profile_uuid!=cell.base_page->page_size_profile_uuid)
                      return fail(E::comment_tree_mismatch);
                    base=&candidate;break;}}
                if(!base)return fail(E::comment_tree_mismatch);
                const auto& metadata=base->page.metadata;
                const auto found=std::lower_bound(metadata.begin(),metadata.end(),cell.key.version_uuid,
                    [](const auto& m,const Uuid& id){return m.version_uuid<id;});
                if(found==metadata.end()||found->version_uuid!=cell.key.version_uuid||found->name)return fail(E::comment_tree_mismatch);
                const auto& row=base->page.body.rows[found->row_index];
                if(row.row_uuid.value!=cell.key.row_uuid||row.local_transaction_id!=cell.creator_local_transaction_id||
                    found->metadata.record.header.kind!=core::catalog::CatalogRecordKind::localized_comment)
                  return fail(E::comment_tree_mismatch);
                const auto comment=core::catalog::DecodeCatalogComment(found->metadata.record.payload);
                if(!comment.ok())return fail(E::comment_tree_mismatch);
                const auto key=core::catalog::CatalogCommentTreeKey(comment.record->target_object_uuid,comment.record->language_uuid);
                if(cell.key.encoded_key.size()!=key.size()||!std::equal(key.begin(),key.end(),cell.key.encoded_key.begin())||
                    !cell.payload.empty())return fail(E::comment_tree_mismatch);
              }}
            usize expected=0;
            for(usize bi=0;bi<bases.size();++bi)for(const auto& m:bases[bi].page.metadata){
              if(m.metadata.record.header.kind!=core::catalog::CatalogRecordKind::localized_comment)continue;
              const bool wants=!m.metadata.record.header.deleted;
              usize matches=0;
              for(const auto n:tree.leaves)for(const auto& cell:tree.pages[n].page->cells)
                if(cell.key.version_uuid==m.version_uuid)++matches;
              if(matches!=(wants?1u:0u)){
                bool admitted=false;
                if(wants&&matches==0)
                  for(const auto& creator:read.source.row_creators){
                    if(creator.catalog_page_index!=bi||creator.catalog_row_index!=m.row_index)continue;
                    if(creator.inventory_entry_index>=inventory.entries.size())break;
                    const auto state=transaction::mga::InventoryVisibilityState(inventory.entries[creator.inventory_entry_index]);
                    admitted=state==transaction::mga::TransactionState::active||
                        state==transaction::mga::TransactionState::rolled_back||
                        state==transaction::mga::TransactionState::failed_terminal;
                    break;
                  }
                if(!admitted)return fail(E::comment_tree_mismatch);
              }
              expected+=matches;
            }
            if(cells!=expected)return fail(E::comment_tree_mismatch);
            out.comment_tree_cells=cells;out.comment_tree_pages=tree.pages.size();
            if(capture){
              for(const auto& page:tree.pages)
                if(!capture->AppendPhysical(page.page->header,dependencies.index_uuid))return fail(E::resource_exhausted);
              capture->comment_tree_=NativeCapturedNameTree{comment_root->kind,dependencies,comment_root->page,cells,tree.pages.size()};
            }
          }
        }
      }
    }
    out.failing_selector=out.failing_role=0;out.source.reset();
    if constexpr(fixed_root)out.source=NativeReadSourceIdentityFromFixedRoot(source.identity());
    if(capture){
      // Preserve the actual selected directory with the same immutable source
      // as the catalog rows. No current device or caller-supplied manifest is
      // substituted after the selected-source fence retires.
      for(const auto& page:view.directory->pages)for(const auto& member:page.directory.records){
        if(capture->filespace_count_==capture->filespaces_.size())return fail(E::resource_exhausted);
        capture->filespaces_[capture->filespace_count_++]=member;
      }
      if(!capture->filespace_count_)return fail(E::binding_mismatch);
      auto members=capture->filespaces_.first(capture->filespace_count_);
      std::sort(members.begin(),members.end(),[](const auto& a,const auto& b){return a.bootstrap.filespace_uuid<b.bootstrap.filespace_uuid;});
      for(usize i=1;i<members.size();++i)if(members[i-1].bootstrap.filespace_uuid==members[i].bootstrap.filespace_uuid)
        return fail(E::binding_mismatch);
      auto physical=capture->physical_pages_.first(capture->physical_page_count_);
      std::sort(physical.begin(),physical.end(),[](const auto& a,const auto& b){return
          a.header.filespace_uuid<b.header.filespace_uuid||
          (a.header.filespace_uuid==b.header.filespace_uuid&&a.header.page_number<b.header.page_number);});
      if(!capture->initial_control_pages_.empty()){
        const auto append=[&](const auto& header,const auto& owner){return capture->AppendPhysicalTo(
            capture->initial_control_pages_,capture->initial_control_page_count_,header,owner);};
        if constexpr(fixed_root){
          // As created: page zero and its root set at generation one. Page
          // zero's roots are the root set; the open verifier reads them from
          // page zero itself, so no checkpoint roots are captured.
          const auto& id=source.identity();
          if(id.page_generation!=1||id.root_set_generation!=1)return fail(E::binding_mismatch);
          for(const auto& page:source.inventory_pages())
            if(!append(page.header,page.object_uuid))return fail(E::resource_exhausted);
        }
        for(const auto& map:view.allocation->pages)
          if(!append(map.map.header,map.map.object_uuid))return fail(E::resource_exhausted);
        for(const auto& page:view.directory->pages)
          if(!append(page.directory.header,page.directory.object_uuid))return fail(E::resource_exhausted);
        auto controls=capture->initial_control_pages_.first(capture->initial_control_page_count_);
        std::sort(controls.begin(),controls.end(),[](const auto& a,const auto& b){return
            a.header.filespace_uuid<b.header.filespace_uuid||
            (a.header.filespace_uuid==b.header.filespace_uuid&&a.header.page_number<b.header.page_number);});
        capture->initial_controls_complete_=true;
      }
      capture->source_=out.source;
    }
    out.error=E::none;return out;
  }
};
} // namespace scratchbird::storage::database
