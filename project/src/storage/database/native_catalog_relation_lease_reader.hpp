// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_roots_lease_reader.hpp"
#include "native_catalog_leaf_lease_reader.hpp"
#include "native_btree_tree_lease_reader.hpp"

namespace scratchbird::storage::database {
struct NativeCatalogRelationMemoryLimits {
  Uuid maximum_profile;
  usize maximum_devices=0,maximum_base_pages=0,maximum_rows=0,maximum_bindings=0;
  NativeBtreeTreeMemoryLimits navigation;
};
struct NativeReadCatalogLeafImage {
  NativeCatalogLeafPageView page;
  std::span<const byte> image;
};
enum class NativeCatalogRelationLeaseError {
  none,invalid_request,memory_failure,root_failure,missing_relation,binding_mismatch,
  tree_failure,leaf_failure,invalid_locator,duplicate_identity,creator_mismatch,
  cluster_requires_authority,resource_exhausted,lock_failure
};
inline usize NativeCatalogRelationWorkspaceBytes(const NativeCatalogRelationMemoryLimits& l) noexcept {
  constexpr auto max=std::numeric_limits<usize>::max();
  if(!l.maximum_base_pages||l.maximum_rows>(max-1)/2)return 0;
  const auto roots=NativeCatalogRootsWorkspaceBytes(l.maximum_profile);
  const auto tree=NativeBtreeTreeWorkspaceBytes(l.maximum_profile,l.maximum_devices,l.navigation);
  const auto leaf=NativeCatalogLeafWorkspaceBytes(l.maximum_profile);
  if(!roots||!tree||!leaf)return 0;
  usize bytes=0;
  const auto add=[&](usize n,usize width,usize pad=0){
    if(n>(max-pad)/width||n*width+pad>max-bytes)return false;
    bytes+=n*width+pad;return true;};
  const auto array=[&]<class T>(usize n){return add(n,sizeof(T),alignof(T)-1);};
  if(!add(1,roots)||!add(1,tree)||!add(l.maximum_base_pages,leaf)||
      !array.template operator()<NativeCatalogLeafPreparedMemory>(l.maximum_base_pages)||
      !array.template operator()<NativeReadCatalogLeafImage>(l.maximum_base_pages)||
      !array.template operator()<NativeCatalogRowImageBinding>(l.maximum_bindings)||
      !array.template operator()<NativeCatalogCreatorBinding>(l.maximum_rows)||
      !array.template operator()<usize>(l.navigation.maximum_pages)||
      !array.template operator()<usize>(2*l.maximum_rows+1))return 0;
  return bytes;
}
struct NativeCatalogRelationPreparedResult;
class NativeCatalogRelationLeaseReader;
class NativeCatalogRelationPreparedMemory {
 public:
  NativeCatalogRelationPreparedMemory() noexcept=default;
  ~NativeCatalogRelationPreparedMemory(){Clear();}
  NativeCatalogRelationPreparedMemory(const NativeCatalogRelationPreparedMemory&)=delete;
  NativeCatalogRelationPreparedMemory& operator=(const NativeCatalogRelationPreparedMemory&)=delete;
  NativeCatalogRelationPreparedMemory(NativeCatalogRelationPreparedMemory&& other) noexcept {Swap(other);}
  NativeCatalogRelationPreparedMemory& operator=(NativeCatalogRelationPreparedMemory&& other) noexcept {
    if(this!=&other){Clear();Swap(other);}return *this;
  }
  explicit operator bool() const noexcept{return bool(arena_)&&bool(roots_)&&bool(tree_)&&!leaves_.empty();}
 private:
  void Clear() noexcept {
    for(auto& leaf:leaves_)std::destroy_at(&leaf);
    leaves_={};arena_={};roots_={};tree_={};
    bases_={};bindings_={};creators_={};navigation_creators_={};versions_={};
  }
  void Swap(NativeCatalogRelationPreparedMemory& other) noexcept {
    using std::swap;swap(arena_,other.arena_);swap(roots_,other.roots_);swap(tree_,other.tree_);
    swap(leaves_,other.leaves_);swap(bases_,other.bases_);swap(bindings_,other.bindings_);
    swap(creators_,other.creators_);swap(navigation_creators_,other.navigation_creators_);swap(versions_,other.versions_);
  }
  NativeStorageArena arena_;
  NativeCatalogRootsPreparedMemory roots_;
  NativeBtreeTreePreparedMemory tree_;
  std::span<NativeCatalogLeafPreparedMemory> leaves_;
  std::span<NativeReadCatalogLeafImage> bases_;
  std::span<NativeCatalogRowImageBinding> bindings_;
  std::span<NativeCatalogCreatorBinding> creators_;
  std::span<usize> navigation_creators_,versions_;
  friend class NativeCatalogRelationLeaseReader;
  friend NativeCatalogRelationPreparedResult PrepareNativeCatalogRelationRead(
    const NativeCatalogRelationMemoryLimits&,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeCatalogRelationPreparedResult {
  NativeCatalogRelationLeaseError error=NativeCatalogRelationLeaseError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeCatalogRelationPreparedMemory workspace;
  bool ok() const noexcept{return error==NativeCatalogRelationLeaseError::none&&bool(workspace);}
};
inline NativeCatalogRelationPreparedResult PrepareNativeCatalogRelationRead(
    const NativeCatalogRelationMemoryLimits& l,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeCatalogRelationLeaseError;NativeCatalogRelationPreparedResult out;
  try {
    const auto total=NativeCatalogRelationWorkspaceBytes(l);if(!total)return out;
    const auto overhead=total-NativeCatalogRootsWorkspaceBytes(l.maximum_profile)-
      NativeBtreeTreeWorkspaceBytes(l.maximum_profile,l.maximum_devices,l.navigation)-
      l.maximum_base_pages*NativeCatalogLeafWorkspaceBytes(l.maximum_profile);
    NativeCatalogRelationPreparedMemory w;
    const auto allocation_failure=[&](auto& r){out.error=E::memory_failure;out.memory_error=r.memory_error;
      out.allocation_status=r.allocation_status;out.allocation_diagnostic=std::move(r.allocation_diagnostic);};
    auto arena=memory.CreateArena(binding,overhead,alignof(std::max_align_t));
    if(!arena.ok()){out.error=E::memory_failure;out.memory_error=arena.error;
      out.allocation_status=arena.backing.status;out.allocation_diagnostic=std::move(arena.backing.diagnostic);return out;}
    w.arena_=std::move(arena.arena);
    const auto array=[&]<class T>(usize n)->std::span<T>{
      static_assert(std::is_trivially_destructible_v<T>);if(!n)return {};
      auto block=w.arena_.Allocate(n*sizeof(T),alignof(T));if(!block.ok())throw std::bad_alloc();
      auto* p=static_cast<T*>(block.pointer);std::uninitialized_value_construct_n(p,n);return {p,n};};
    const auto slots=w.arena_.Allocate(l.maximum_base_pages*sizeof(NativeCatalogLeafPreparedMemory),alignof(NativeCatalogLeafPreparedMemory));
    if(!slots.ok())throw std::bad_alloc();
    auto* leaf_slots=static_cast<NativeCatalogLeafPreparedMemory*>(slots.pointer);
    std::uninitialized_value_construct_n(leaf_slots,l.maximum_base_pages);w.leaves_={leaf_slots,l.maximum_base_pages};
    w.bases_=array.template operator()<NativeReadCatalogLeafImage>(l.maximum_base_pages);
    w.bindings_=array.template operator()<NativeCatalogRowImageBinding>(l.maximum_bindings);
    w.creators_=array.template operator()<NativeCatalogCreatorBinding>(l.maximum_rows);
    w.navigation_creators_=array.template operator()<usize>(l.navigation.maximum_pages);
    w.versions_=array.template operator()<usize>(2*l.maximum_rows+1);
    auto roots=PrepareNativeCatalogRootsRead(l.maximum_profile,memory,binding);
    if(!roots.ok()){allocation_failure(roots);return out;}w.roots_=std::move(roots.workspace);
    auto tree=PrepareNativeBtreeTreeRead(l.maximum_profile,l.maximum_devices,l.navigation,memory,binding);
    if(!tree.ok()){allocation_failure(tree);return out;}w.tree_=std::move(tree.workspace);
    for(auto& slot:w.leaves_){
      auto leaf=PrepareNativeCatalogLeafRead(l.maximum_profile,memory,binding,NativeCatalogLeafProfileMode::maximum);
      if(!leaf.ok()){allocation_failure(leaf);return out;}slot=std::move(leaf.workspace);
    }
    out.workspace=std::move(w);out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
struct NativeCatalogRelationLeaseResult {
  NativeCatalogRelationLeaseError error=NativeCatalogRelationLeaseError::invalid_request;
  NativeCatalogRootsLeaseReadResult roots;
  NativeBtreeTreeReadResult tree;
  NativeCatalogLeafLeaseReadResult leaf_failure;
  std::span<const NativeReadCatalogLeafImage> bases;
  std::span<const NativeCatalogRowImageBinding> bindings;
  std::span<const NativeCatalogCreatorBinding> row_creators;
  std::span<const usize> navigation_creators;
  u64 physical_bytes_read=0,retained_image_bytes=0;
  bool ok() const noexcept{return error==NativeCatalogRelationLeaseError::none&&roots.ok();}
};
// Selected-root and complete image/creator binding only, NOT row visibility,
// dependency-map/key admission, family/security, allocation or publication.
class NativeCatalogRelationLeaseReader {
 public:
  static NativeCatalogRelationLeaseResult Read(const NativeSelectedCheckpointMemoryLease& source,
      u16 selector,u16 role,const NativeCatalogRelationBinding& binding,u64 maximum_total_image_bytes,
      NativeCatalogRelationPreparedMemory& w) noexcept {
    using E=NativeCatalogRelationLeaseError;NativeCatalogRelationLeaseResult out;
    const auto fail=[&](E e){out.error=e;out.roots.catalogs={};out.roots.retained_image_bytes=0;out.roots.feature_root_index=0;
      out.tree.pages={};out.tree.leaves={};out.tree.retained_image_bytes=0;
      out.leaf_failure.image={};out.leaf_failure.leaf.page.reset();out.bases={};out.bindings={};
      out.row_creators={};out.navigation_creators={};out.retained_image_bytes=0;};
    try {
      if(!w||(selector!=2&&selector!=8)||role<1||role>6||!core::uuid::IsEngineIdentityUuid(binding.relation_uuid))return out;
      if(w.arena_.binding().database_uuid!=source.selection().checkpoint_inventory.checkpoint->header.database_uuid)return out;
      u64 retained=source.retained_image_bytes();
      if(retained>=maximum_total_image_bytes){fail(E::resource_exhausted);return out;}
      out.roots=NativeCatalogRootsLeaseReader::Read(source,maximum_total_image_bytes-retained,w.roots_);
      out.physical_bytes_read+=out.roots.physical_bytes_read;
      if(!out.roots.ok()){fail(E::root_failure);return out;}
      retained+=out.roots.retained_image_bytes;
      const auto& catalog=out.roots.catalogs[selector==2?0:out.roots.feature_root_index].root;
      const auto head=std::find_if(catalog.roots.begin(),catalog.roots.end(),[&](const auto& r){return r.role==role;});
      if(head==catalog.roots.end()){fail(E::missing_relation);return out;}
      const bool indexed=head->page_type==0x200;
      if((!indexed&&head->page_type!=6)||
          (indexed?(!binding.index_dependencies||binding.index_dependencies->index_uuid!=head->object_uuid):
            (binding.index_dependencies.has_value()||head->object_uuid!=binding.relation_uuid))){fail(E::binding_mismatch);return out;}
      if(indexed){
        out.tree=NativeBtreeTreeLeaseReader::Read(source,head->page,*binding.index_dependencies,w.tree_,
          static_cast<usize>(std::min<u64>(maximum_total_image_bytes-retained,std::numeric_limits<usize>::max())));
        out.physical_bytes_read+=out.tree.physical_bytes_read;
        if(!out.tree.ok()){fail(E::tree_failure);return out;}retained+=out.tree.retained_image_bytes;
      }
      const auto& inventory=source.selection().checkpoint_inventory.inventory.entries;
      E failure=E::none;usize page_count=0,row_count=0,binding_count=0;
      constexpr auto missing=std::numeric_limits<usize>::max();
      std::fill(w.versions_.begin(),w.versions_.end(),missing);
      const auto creator=[&](u64 number,const Uuid& uuid)->usize{
        const auto entry=std::lower_bound(inventory.begin(),inventory.end(),number,[](const auto& r,u64 n){return r.identity.local_id.value<n;});
        if(entry==inventory.end()||entry->identity.local_id.value!=number||entry->identity.transaction_uuid.value!=uuid){failure=E::creator_mismatch;return missing;}
        if(entry->identity.scope!=transaction::mga::TransactionScope::local_node){failure=E::cluster_requires_authority;return missing;}
        return static_cast<usize>(entry-inventory.begin());
      };
      for(usize i=0;i<out.tree.pages.size();++i){const auto& p=*out.tree.pages[i].page;
        const auto at=creator(p.creator_local_transaction_id,p.creator_transaction_uuid);
        if(at==missing){fail(failure);return out;}w.navigation_creators_[i]=at;}
      const auto same_slot=[](const auto& h,const auto& r){return h.filespace_uuid==r.filespace_uuid&&h.page_number==r.page_number;};
      const auto load=[&](const disk::NativePageReference& ref)->usize{
        for(const auto& p:out.tree.pages)if(same_slot(p.page->header,ref)){failure=E::invalid_locator;return missing;}
        for(usize i=0;i<page_count;++i)if(same_slot(w.bases_[i].page.header,ref)){
          const auto& h=w.bases_[i].page.header;
          if(h.page_generation!=ref.page_generation||h.page_size_profile_uuid!=ref.page_size_profile_uuid){failure=E::binding_mismatch;return missing;}return i;}
        const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
        if(!profile){failure=E::invalid_locator;return missing;}
        if(page_count==w.bases_.size()||profile->page_size_bytes>maximum_total_image_bytes-retained){failure=E::resource_exhausted;return missing;}
        auto leaf=NativeCatalogLeafLeaseReader::Read(source,{role,6,ref,binding.relation_uuid},w.leaves_[page_count]);
        out.physical_bytes_read+=leaf.bytes_read;
        if(!leaf.ok()){out.leaf_failure=std::move(leaf);failure=E::leaf_failure;return missing;}
        const auto& p=*leaf.leaf.page;
        for(const auto& n:out.tree.pages)if(n.page->header.page_uuid==p.header.page_uuid){failure=E::duplicate_identity;return missing;}
        for(usize i=0;i<page_count;++i)if(w.bases_[i].page.header.page_uuid==p.header.page_uuid){failure=E::duplicate_identity;return missing;}
        if(p.body.rows.size()>w.creators_.size()-row_count){failure=E::resource_exhausted;return missing;}
        w.bases_[page_count]={p,leaf.image};
        for(usize row_index=0;row_index<p.body.rows.size();++row_index){const auto& row=p.body.rows[row_index];
          const auto metadata=std::lower_bound(p.metadata.begin(),p.metadata.end(),row.version_uuid,
            [](const auto& m,const auto& id){return m.version_uuid<id;});
          if(metadata->metadata.authority_scope==core::catalog::CatalogAuthorityScope::cluster){failure=E::cluster_requires_authority;return missing;}
          const auto at=creator(row.local_transaction_id,row.transaction_uuid.value);if(at==missing)return missing;
          auto slot=btree_memory_detail::Hash(row.version_uuid)%w.versions_.size();
          while(w.versions_[slot]!=missing){const auto& prior=w.creators_[w.versions_[slot]];
            if(w.bases_[prior.catalog_page_index].page.body.rows[prior.catalog_row_index].version_uuid==row.version_uuid){failure=E::duplicate_identity;return missing;}
            if(++slot==w.versions_.size())slot=0;}
          w.versions_[slot]=row_count;w.creators_[row_count++]={page_count,row_index,at};
        }
        retained+=leaf.image.size();return page_count++;
      };
      const auto append=[&](NativeCatalogRowImageBinding b){if(binding_count==w.bindings_.size()){failure=E::resource_exhausted;return false;}
        w.bindings_[binding_count++]=b;return true;};
      if(!indexed){const auto p=load(head->page);if(p==missing){fail(failure);return out;}
        for(usize r=0;r<w.bases_[p].page.body.rows.size();++r)if(!append({{},p,r})){fail(failure);return out;}
      }else for(const auto n:out.tree.leaves){const auto& page=*out.tree.pages[n].page;
        for(usize i=0;i<page.cells.size();++i){const auto& cell=page.cells[i];
          if(!cell.base_page){fail(E::invalid_locator);return out;}
          const auto p=load(*cell.base_page);if(p==missing){fail(failure);return out;}
          const auto& leaf=w.bases_[p].page;
          const auto row=std::lower_bound(leaf.metadata.begin(),leaf.metadata.end(),cell.key.version_uuid,
            [](const auto& m,const auto& id){return m.version_uuid<id;});
          if(row==leaf.metadata.end()||row->version_uuid!=cell.key.version_uuid){fail(E::invalid_locator);return out;}
          if(leaf.body.rows[row->row_index].row_uuid.value!=cell.key.row_uuid){fail(E::binding_mismatch);return out;}
          if(!append({NativeCatalogIndexEntryLocation{n,i},p,row->row_index})){fail(failure);return out;}
        }
      }
      out.bases=w.bases_.first(page_count);out.bindings=w.bindings_.first(binding_count);
      out.row_creators=w.creators_.first(row_count);out.navigation_creators=w.navigation_creators_.first(out.tree.pages.size());
      out.retained_image_bytes=retained;out.error=E::none;return out;
    }catch(const std::bad_alloc&){fail(E::resource_exhausted);}
     catch(const std::length_error&){fail(E::resource_exhausted);}
     catch(const std::system_error&){fail(E::lock_failure);}
    return out;
  }
};
} // namespace scratchbird::storage::database
