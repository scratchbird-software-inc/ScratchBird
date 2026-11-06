// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_lease_read_source.hpp"
#include <concepts>

namespace scratchbird::storage::database {
enum class NativeCatalogCohortError {
  none,invalid_request,root_failure,relation_failure,unresolved_creator,
  binding_mismatch,resource_exhausted
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
  std::array<NativeCatalogRelationInspection,12> relations{};
  usize relation_count=0;
  u16 failing_selector=0,failing_role=0;
  Uuid unresolved_transaction_uuid;
  u64 unresolved_local_transaction_id=0,physical_bytes_read=0,verified_image_bytes=0;
  bool ok() const noexcept{return error==NativeCatalogCohortError::none&&source.has_value();}
};
struct NativeCatalogCohortPreparedMemory {
  NativeCatalogRootsPreparedMemory roots;
  NativeCatalogVisibilityPreparedMemory visibility;
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
  return roots&&visibility&&roots<=SIZE_MAX-visibility?roots+visibility:0;
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
  out.error=NativeCatalogRelationLeaseError::none;return out;
}
// Actual selected catalog/navigation/base-leaf coverage, metadata chains and
// committed row visibility. This is a verifier input, not a complete engine
// verification receipt: cross-family semantic references, user relations,
// constraints, recovery decisions and service admission remain engine-owned.
class NativeCatalogCohortLeaseReader {
 public:
  // SB_NATIVE_FIXED_ROOT_READ: through the selected checkpoint or page zero's
  // fixed roots. Database, inventory, allocation and directory come from the
  // same lease-neutral view; only the creation graph differs (see below).
  template<class Lease>
    requires std::same_as<Lease,NativeSelectedCheckpointMemoryLease>||std::same_as<Lease,NativeFixedRootMemoryLease>
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
          if(state!=transaction::mga::TransactionState::committed&&state!=transaction::mga::TransactionState::rolled_back&&
              state!=transaction::mga::TransactionState::failed_terminal){
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
      }
    }
    out.failing_selector=out.failing_role=0;out.source.reset();
    if constexpr(fixed_root)out.source=NativeReadSourceIdentityFromFixedRoot(source.identity());
    else if(source.selection().selection)out.source=NativeReadSourceIdentityFromSelection(*source.selection().selection);
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
          capture->initial_control_root_count_=0;
          for(const auto& page:source.inventory_pages())
            if(!append(page.header,page.object_uuid))return fail(E::resource_exhausted);
        }else{
          const auto& selected=source.selection();const auto& checkpoint=*selected.checkpoint_inventory.checkpoint;
          if(selected.selection->selection_generation!=1||checkpoint.predecessor||checkpoint.checkpoint_generation!=1||
              checkpoint.roots.size()>capture->initial_control_roots_.size())return fail(E::binding_mismatch);
          std::copy(checkpoint.roots.begin(),checkpoint.roots.end(),capture->initial_control_roots_.begin());
          capture->initial_control_root_count_=checkpoint.roots.size();
          if(!append(checkpoint.header,checkpoint.object_uuid))return fail(E::resource_exhausted);
          for(const auto& page:selected.checkpoint_inventory.inventory_pages)
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
