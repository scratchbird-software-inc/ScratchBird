// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_catalog_visibility_stream_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_lease_read_source.hpp"
#include "catalog_comment_record_codec.hpp"
#include <concepts>
#include <numeric>

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
  // SB_OPEN_SIZED_FROM_FILE: the failed relation's own index tree read result (resource_exhausted: the navigation bound filled).
  NativeBtreeTreeMemoryError relation_tree_error=NativeBtreeTreeMemoryError::none;
  page::NativeBtreeError name_tree_page_error=page::NativeBtreeError::none;
  u16 failing_selector=0,failing_role=0;
  // SB_NATIVE_CATALOG_CAPTURE_BOUND (owner Q219 = yes, 2026-11-15): on
  // resource_exhausted, the bound that filled (rows or text bytes, and so on);
  // failing_role / failing_selector name the relation being read when it
  // filled (0: none, the inventory, filespaces or control pages). none on
  // every other result.
  NativeCatalogCaptureBound capture_bound=NativeCatalogCaptureBound::none;
  Uuid unresolved_transaction_uuid;
  u64 unresolved_local_transaction_id=0,physical_bytes_read=0,verified_image_bytes=0;
  // SB_OPEN_CAPTURE_EXACT (2026-11-27): what a capture of this very read needs, counted whether or not one was asked for, so the
  // open can size its capture from the scan instead of doubling a fixed first guess: visible rows, the bytes copied for them (row
  // payloads, name texts, name-tree cell keys and payloads), name-tree cells, and physical pages (roots, relation and index pages).
  u64 needed_rows=0,needed_text_bytes=0,needed_name_tree_cells=0,needed_physical_pages=0;
  bool ok() const noexcept{return error==NativeCatalogCohortError::none&&source.has_value();}
};
// SEARCH_KEY: SB_OPEN_COHORT_STREAM (2026-11-27, owner 2026-11-25/26: "a streaming reader/writer is a high priority"). The open's
// cohort scan reads every relation through the streaming visibility reader: a window of decoded base leaves, a cursor over the
// index, hooks for the checks the collect-everything readers made over every stored version, and sorted or hashed passes over
// small per-version and per-page keys for the checks that are relation-wide. What a relation costs in memory is the window (a
// fixed size) plus those KEYS (about 200 bytes per row version and 100 per page), no longer one decoded leaf workspace per page
// (about 70 KiB at 8 KiB pages). NativeCatalogRelationMemoryLimits keeps its meaning as the SIZE BOUNDS: maximum_rows bounds the
// row versions of a relation (and the history registry), maximum_base_pages its distinct base leaves, navigation its index
// pages, name_trees one name tree's pages; a bound that fills is resource_exhausted exactly as before, so the open's retry at a
// larger scale still applies, but a scale step now costs kilobytes of keys instead of tens of megabytes of leaf workspace.
namespace cohort_detail {
// One stored row version of the relation being read, as the open's relation-wide checks need it.
struct VersionKey {
  Uuid version,row,meta_row,object,previous_version;
  u64 row_version=0,previous_row_version=0,creator_local=0;
  u32 page=0,row_index=0,creator_index=0;
  u16 kind=0;
  u8 flags=0;
  static constexpr u8 deleted=1,name=2,entry=4,comment=8,comment_decoded=16;
};
// One distinct base leaf of the relation.
struct PageKey {
  Uuid filespace,page_uuid,profile;
  u64 page_number=0,generation=0;
  u32 size=0;
};
// One index page (of the relation's index, or of one page-zero tree).
struct TreePageKey {
  Uuid filespace,page_uuid;
  u64 page_number=0;
};
struct CommentKey {Uuid target,language;};
constexpr u32 kNone=std::numeric_limits<u32>::max();
// The verifier-retained keys of ONE relation (rebuilt for each; reused by every relation of the open).
struct Verifier {
  NativeStorageArena arena;
  std::span<VersionKey> versions;
  std::span<u32> version_hash,page_hash;
  std::span<PageKey> pages;
  std::span<TreePageKey> tree_pages;
  std::span<CommentKey> comment_keys;
  std::span<usize> order_a,order_b,page_order;
  std::span<u32> counters;
  explicit operator bool() const noexcept{return bool(arena)&&!versions.empty()&&!pages.empty();}
};
inline usize TreePageCapacity(const NativeCatalogRelationMemoryLimits& l) noexcept {
  return std::max<usize>({l.navigation.maximum_pages,l.name_trees.maximum_pages,usize{1}});
}
inline usize VerifierBytes(const NativeCatalogRelationMemoryLimits& l) noexcept {
  constexpr auto max=std::numeric_limits<usize>::max();
  if(!l.maximum_rows||!l.maximum_base_pages||l.maximum_rows>(max-1)/2||l.maximum_base_pages>(max-1)/2)return 0;
  usize bytes=0;bool ok=true;
  const auto add=[&]<class T>(usize n){
    const auto pad=alignof(T)-1;
    if(n>(max-pad)/sizeof(T)||n*sizeof(T)+pad>max-bytes){ok=false;return;}
    bytes+=n*sizeof(T)+pad;};
  const auto rows=l.maximum_rows,pages=l.maximum_base_pages,tree=TreePageCapacity(l);
  if(pages>max-tree)return 0;
  add.template operator()<VersionKey>(rows);add.template operator()<u32>(2*rows+1);
  add.template operator()<PageKey>(pages);add.template operator()<u32>(2*pages+1);
  add.template operator()<TreePageKey>(tree);add.template operator()<CommentKey>(rows);
  add.template operator()<usize>(rows);add.template operator()<usize>(rows);add.template operator()<usize>(pages+tree);
  add.template operator()<u32>(rows);
  return ok?bytes:0;
}
// The window of decoded base leaves the pass keeps: a bounded size, whatever the file's (an 8 MiB window of leaf workspaces,
// never fewer than eight leaves and never more than the relation's page bound).
inline constexpr usize kCohortWindowBytes=8u*1024u*1024u;
inline NativeCatalogStreamLimits StreamLimitsFor(const NativeCatalogRelationMemoryLimits& l) noexcept {
  NativeCatalogStreamLimits s;
  s.maximum_profile=l.maximum_profile;s.maximum_devices=l.maximum_devices?l.maximum_devices:1;
  const auto leaf=NativeCatalogLeafWorkspaceBytes(l.maximum_profile);
  usize window=leaf?kCohortWindowBytes/leaf:8;
  window=std::max<usize>(window,8);
  if(l.maximum_base_pages)window=std::min<usize>(window,std::max<usize>(l.maximum_base_pages,8));
  s.window_pages=window;
  // Rows with more than one version are registered until the pass ends (MGA history a sweep removes); the row bound is the bound.
  s.history_versions=std::max<usize>(l.maximum_rows,1);
  return s;
}
}  // namespace cohort_detail
struct NativeCatalogCohortPreparedMemory {
  NativeCatalogRootsPreparedMemory roots;
  NativeCatalogStreamPreparedMemory stream;
  cohort_detail::Verifier verifier;
  // SB_NATIVE_NAME_TREES: whether the limits asked for name trees (one tree at a time, through the stream's own cursor); and
  // the bounds one tree's pass keeps (pages and page-image bytes).
  bool name_trees=false;
  usize name_tree_pages=0;u64 name_tree_image_bytes=0;
  // The relation's own index bounds (pages and page-image bytes), as the limits gave them.
  usize navigation_pages=0;u64 navigation_image_bytes=0;
  explicit operator bool() const noexcept{return bool(roots)&&bool(stream)&&bool(verifier);}
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
  const auto stream=NativeCatalogStreamWorkspaceBytes(cohort_detail::StreamLimitsFor(limits));
  const auto verifier=cohort_detail::VerifierBytes(limits);
  constexpr auto max=std::numeric_limits<usize>::max();
  if(!roots||!stream||!verifier||roots>max-stream||roots+stream>max-verifier)return 0;
  return roots+stream+verifier;
}
// One reusable, admitted relation workspace covers every declared role. It is
// prepared before source guards and reused only after each role's complete
// observation has been consumed. No row text or borrowed mutable view escapes.
inline NativeCatalogCohortPreparedResult PrepareNativeCatalogCohortRead(
    const NativeCatalogRelationMemoryLimits& limits,NativeStorageMemory& memory,
    const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeCatalogRelationLeaseError;
  NativeCatalogCohortPreparedResult out;
  try {
    if(!NativeCatalogCohortWorkspaceBytes(limits))return out;
    auto root=PrepareNativeCatalogRootsRead(limits.maximum_profile,memory,binding);
    out.memory_error=root.memory_error;out.allocation_status=root.allocation_status;
    out.allocation_diagnostic=std::move(root.allocation_diagnostic);
    if(!root.ok()){out.error=E::memory_failure;return out;}
    auto stream=PrepareNativeCatalogStreamRead(cohort_detail::StreamLimitsFor(limits),memory,binding);
    out.memory_error=stream.memory_error;
    if(!stream.ok()){out.error=stream.error==NativeCatalogStreamError::resource_exhausted?E::resource_exhausted:E::memory_failure;return out;}
    const auto bytes=cohort_detail::VerifierBytes(limits);
    auto arena=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
    out.memory_error=arena.error;out.allocation_status=arena.backing.status;
    out.allocation_diagnostic=std::move(arena.backing.diagnostic);
    if(!arena.ok()){out.error=E::memory_failure;return out;}
    auto& v=out.workspace.verifier;
    v.arena=std::move(arena.arena);
    const auto array=[&]<class T>(usize n)->std::span<T>{
      static_assert(std::is_trivially_destructible_v<T>);
      if(!n)return {};
      auto block=v.arena.Allocate(n*sizeof(T),alignof(T));
      if(!block.ok())throw std::bad_alloc();
      auto* p=static_cast<T*>(block.pointer);std::uninitialized_value_construct_n(p,n);return {p,n};};
    const auto rows=limits.maximum_rows,pages=limits.maximum_base_pages,tree=cohort_detail::TreePageCapacity(limits);
    v.versions=array.template operator()<cohort_detail::VersionKey>(rows);
    v.version_hash=array.template operator()<u32>(2*rows+1);
    v.pages=array.template operator()<cohort_detail::PageKey>(pages);
    v.page_hash=array.template operator()<u32>(2*pages+1);
    v.tree_pages=array.template operator()<cohort_detail::TreePageKey>(tree);
    v.comment_keys=array.template operator()<cohort_detail::CommentKey>(rows);
    v.order_a=array.template operator()<usize>(rows);v.order_b=array.template operator()<usize>(rows);
    v.page_order=array.template operator()<usize>(pages+tree);
    v.counters=array.template operator()<u32>(rows);
    out.workspace.roots=std::move(root.workspace);out.workspace.stream=std::move(stream.workspace);
    out.workspace.name_trees=limits.name_trees.maximum_pages!=0;
    out.workspace.name_tree_pages=limits.name_trees.maximum_pages;
    out.workspace.name_tree_image_bytes=limits.name_trees.maximum_retained_image_bytes;
    out.workspace.navigation_pages=limits.navigation.maximum_pages;
    out.workspace.navigation_image_bytes=limits.navigation.maximum_retained_image_bytes;
    out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
  return out;
}
// Actual selected catalog/navigation/base-leaf coverage, metadata chains and
// committed row visibility. This is a verifier input, not a complete engine
// verification receipt: cross-family semantic references, user relations,
// constraints, recovery decisions and service admission remain engine-owned.
class NativeCatalogCohortLeaseReader {
  using Bound=NativeCatalogCaptureBound;
  using VK=cohort_detail::VersionKey;
  using PK=cohort_detail::PageKey;
  // The hooks one relation's (or one page-zero tree's) pass runs: the per-page, per-version and per-tree-page checks of the
  // collect-everything readers (see the refusal inventory in the commit message), plus the keys the relation-wide checks sort.
  class Pass {
   public:
    static constexpr bool kActive=true;
    static constexpr bool kAllHistoryGroups=true;
    struct Failure {
      NativeCatalogCohortError cohort=NativeCatalogCohortError::none;
      NativeCatalogRelationLeaseError relation=NativeCatalogRelationLeaseError::none;
      NativePinnedCatalogReadError visibility=NativePinnedCatalogReadError::none;
      NativeBtreeTreeMemoryError tree=NativeBtreeTreeMemoryError::none;
      page::NativeBtreeError tree_page=page::NativeBtreeError::none;
      Bound bound=Bound::none;
      Uuid unresolved_transaction;u64 unresolved_local=0;
      bool set=false;
    };
    Pass(cohort_detail::Verifier& verifier,NativeCatalogCapture* capture,
        std::span<const transaction::mga::TransactionInventoryEntry> inventory,u64 allowance,
        usize tree_page_limit,u64 tree_image_limit,bool reset=true) noexcept
      : v_(verifier),capture_(capture),inventory_(inventory),allowance_(allowance),
        tree_page_limit_(tree_page_limit),tree_image_limit_(tree_image_limit) {
      // (A name-tree pass keeps the relation's registries: its cells bind to them.)
      if(reset){
        std::fill(v_.version_hash.begin(),v_.version_hash.end(),cohort_detail::kNone);
        std::fill(v_.page_hash.begin(),v_.page_hash.end(),cohort_detail::kNone);
      }
    }
    bool on_leaf(const disk::NativePageReference& ref,const NativeCatalogLeafPageView& page) noexcept {
      namespace mga=transaction::mga;
      const auto& h=page.header;
      // A page registered already (the window evicted it and read it again): its rows were verified when it first passed.
      if(const auto known=FindPage(ref.filespace_uuid,ref.page_number);known!=cohort_detail::kNone){
        const auto& p=v_.pages[known];
        if(p.page_uuid==h.page_uuid&&p.generation==h.page_generation)return true;
        return Refuse(NativeCatalogRelationLeaseError::binding_mismatch);
      }
      if(pages_==v_.pages.size())return Refuse(NativeCatalogRelationLeaseError::resource_exhausted);
      if(!Charge(h.page_size_bytes))return false;
      if(capture_)if(const auto b=capture_->AppendPhysicalUnchecked(h,page.body.relation_uuid.value);b!=Bound::none)return RefuseBound(b);
      if(page.body.rows.size()>v_.versions.size()-versions_)return Refuse(NativeCatalogRelationLeaseError::resource_exhausted);
      const u32 page_index=static_cast<u32>(pages_);
      {auto slot=Slot(ref.filespace_uuid,ref.page_number,v_.page_hash.size());
        while(v_.page_hash[slot]!=cohort_detail::kNone)if(++slot==v_.page_hash.size())slot=0;
        v_.page_hash[slot]=page_index;}
      v_.pages[pages_++]={ref.filespace_uuid,h.page_uuid,h.page_size_profile_uuid,ref.page_number,h.page_generation,h.page_size_bytes};
      max_page_bytes_=std::max<usize>(max_page_bytes_,h.page_size_bytes);
      for(usize row_index=0;row_index<page.body.rows.size();++row_index){
        const auto& row=page.body.rows[row_index];
        const auto found=std::lower_bound(page.metadata.begin(),page.metadata.end(),row.version_uuid,
            [](const auto& m,const auto& id){return m.version_uuid<id;});
        if(found==page.metadata.end()||found->version_uuid!=row.version_uuid)return Refuse(NativeCatalogRelationLeaseError::invalid_locator);
        const auto& record=*found;
        if(record.metadata.authority_scope==core::catalog::CatalogAuthorityScope::cluster)
          return Refuse(NativeCatalogRelationLeaseError::cluster_requires_authority);
        const auto entry=std::lower_bound(inventory_.begin(),inventory_.end(),row.local_transaction_id,
            [](const auto& r,u64 n){return r.identity.local_id.value<n;});
        if(entry==inventory_.end()||entry->identity.local_id.value!=row.local_transaction_id||
            entry->identity.transaction_uuid.value!=row.transaction_uuid.value)return Refuse(NativeCatalogRelationLeaseError::creator_mismatch);
        if(entry->identity.scope!=mga::TransactionScope::local_node)return Refuse(NativeCatalogRelationLeaseError::cluster_requires_authority);
        // The metadata the visibility decision judges (every stored version, as the collect-everything reader checked them).
        if(!mga::ObserveRowVersionMetadata(NativeCatalogVisibilityLeaseReader::VersionMetadataOf(row,*entry)).ok()){
          failure_.visibility=NativePinnedCatalogReadError::visibility_failure;failure_.cohort=NativeCatalogCohortError::relation_failure;
          failure_.set=true;return false;}
        // An in-doubt or transient creator stays unresolved; in progress, committed, rolled back and failed are admitted.
        const auto state=mga::InventoryVisibilityState(*entry);
        if(state!=mga::TransactionState::committed&&state!=mga::TransactionState::rolled_back&&
            state!=mga::TransactionState::failed_terminal&&state!=mga::TransactionState::active&&
            state!=mga::TransactionState::read_only_active){
          failure_.cohort=NativeCatalogCohortError::unresolved_creator;failure_.unresolved_transaction=entry->identity.transaction_uuid.value;
          failure_.unresolved_local=entry->identity.local_id.value;failure_.set=true;return false;}
        auto slot=btree_memory_detail::Hash(row.version_uuid)%v_.version_hash.size();
        while(v_.version_hash[slot]!=cohort_detail::kNone)
          {if(v_.versions[v_.version_hash[slot]].version==row.version_uuid)return Refuse(NativeCatalogRelationLeaseError::duplicate_identity);
           if(++slot==v_.version_hash.size())slot=0;}
        const auto& header=record.metadata.record.header;
        VK key;
        key.version=row.version_uuid;key.row=row.row_uuid.value;key.meta_row=header.row_uuid.value;key.object=header.object_uuid.value;
        key.previous_version=row.previous_version_uuid;key.row_version=row.row_version;key.previous_row_version=row.previous_row_version;
        key.creator_local=row.local_transaction_id;key.page=page_index;key.row_index=static_cast<u32>(row_index);
        key.creator_index=static_cast<u32>(entry-inventory_.begin());key.kind=static_cast<u16>(header.kind);
        key.flags=(header.deleted?VK::deleted:0)|(record.name?VK::name:0)|
            (record.name&&std::holds_alternative<core::catalog::CatalogNameEntryView>(*record.name)?VK::entry:0);
        if(header.kind==core::catalog::CatalogRecordKind::localized_comment){
          key.flags|=VK::comment;
          const auto comment=core::catalog::DecodeCatalogComment(record.metadata.record.payload);
          if(comment.ok()){key.flags|=VK::comment_decoded;v_.comment_keys[versions_]={comment.record->target_object_uuid,comment.record->language_uuid};}
        }
        v_.version_hash[slot]=static_cast<u32>(versions_);
        v_.versions[versions_++]=key;
      }
      return true;
    }
    bool on_tree_page(const disk::NativePageReference& ref,const page::NativeBtreePageView& p,bool root) noexcept {
      if(tree_pages_==v_.tree_pages.size()||tree_pages_>=tree_page_limit_){
        failure_.tree=NativeBtreeTreeMemoryError::resource_exhausted;failure_.tree_page=page::NativeBtreeError::resource_exhausted;
        failure_.relation=NativeCatalogRelationLeaseError::tree_failure;failure_.set=true;return false;}
      if(!Charge(p.header.page_size_bytes))return false;
      tree_image_bytes_+=p.header.page_size_bytes;
      if(tree_image_bytes_>tree_image_limit_){
        failure_.tree=NativeBtreeTreeMemoryError::resource_exhausted;failure_.tree_page=page::NativeBtreeError::resource_exhausted;
        failure_.relation=NativeCatalogRelationLeaseError::tree_failure;failure_.set=true;return false;}
      if(capture_)if(const auto b=capture_->AppendPhysicalUnchecked(p.header,p.dependencies.index_uuid);b!=Bound::none)return RefuseBound(b);
      v_.tree_pages[tree_pages_++]={ref.filespace_uuid,p.header.page_uuid,ref.page_number};
      max_page_bytes_=std::max<usize>(max_page_bytes_,p.header.page_size_bytes);
      if(root){dependencies_=p.dependencies;root_page_=ref;}
      return true;
    }
    // A bound or a check that ended the pass; false from a hook is only returned after one of these is set.
    const Failure& failure() const noexcept{return failure_;}
    usize versions() const noexcept{return versions_;}
    usize pages() const noexcept{return pages_;}
    usize tree_pages() const noexcept{return tree_pages_;}
    u64 charged_bytes() const noexcept{return charged_;}
    u64 tree_image_bytes() const noexcept{return tree_image_bytes_;}
    usize max_page_bytes() const noexcept{return max_page_bytes_;}
    const std::optional<page::NativeBtreeDependencies>& dependencies() const noexcept{return dependencies_;}
    const disk::NativePageReference& root_page() const noexcept{return root_page_;}
    u32 FindPage(const Uuid& filespace,u64 number) const noexcept {
      auto slot=btree_memory_detail::Hash(filespace,number)%v_.page_hash.size();
      while(v_.page_hash[slot]!=cohort_detail::kNone){
        const auto& p=v_.pages[v_.page_hash[slot]];
        if(p.page_number==number&&p.filespace==filespace)return v_.page_hash[slot];
        if(++slot==v_.page_hash.size())slot=0;}
      return cohort_detail::kNone;
    }
    bool Charge(u64 bytes) noexcept {
      charged_+=bytes;
      if(charged_>allowance_){failure_.bound=Bound::image_bytes;failure_.cohort=NativeCatalogCohortError::resource_exhausted;failure_.set=true;return false;}
      return true;
    }
    bool RefuseBound(Bound b) noexcept{failure_.bound=b;failure_.cohort=NativeCatalogCohortError::resource_exhausted;failure_.set=true;return false;}
   private:
    bool Refuse(NativeCatalogRelationLeaseError e) noexcept {
      failure_.relation=e;failure_.cohort=NativeCatalogCohortError::relation_failure;failure_.set=true;return false;}
    static u64 Slot(const Uuid& filespace,u64 number,usize size) noexcept{return btree_memory_detail::Hash(filespace,number)%size;}
    cohort_detail::Verifier& v_;
    NativeCatalogCapture* capture_;
    std::span<const transaction::mga::TransactionInventoryEntry> inventory_;
    u64 allowance_=0,charged_=0,tree_image_bytes_=0,tree_image_limit_=0;
    usize tree_page_limit_=0,pages_=0,versions_=0,tree_pages_=0,max_page_bytes_=0;
    std::optional<page::NativeBtreeDependencies> dependencies_;
    disk::NativePageReference root_page_;
    Failure failure_;
  };
  // ---- the relation-wide checks, as sorted passes over the version keys (they need every version at once) ----
  // duplicate object reservation, a row's versions (distinct row versions, one object and kind), a previous link that names
  // another row's version or the wrong row version, and the sequence check. O(n log n) over two index arrays; the version
  // order (by version UUID) is left in order_a for the name-tree binding. (A version UUID repeated across rows was refused
  // when the second was registered.) The chain rules between versions of one row (names preserved, family origin, definition
  // version succession, epochs) run in the stream's own history pass, for every group (kAllHistoryGroups).
  static NativePinnedCatalogReadError RelationWide(cohort_detail::Verifier& v,usize n,
      std::span<const transaction::mga::TransactionInventoryEntry> inventory) noexcept {
    namespace mga=transaction::mga;using E=NativePinnedCatalogReadError;
    constexpr auto absent=std::numeric_limits<usize>::max();
    auto ver=v.versions.first(n);
    auto by_version=v.order_a.first(n);auto order=v.order_b.first(n);
    std::iota(by_version.begin(),by_version.end(),usize{0});
    std::sort(by_version.begin(),by_version.end(),[&](usize a,usize b){return ver[a].version<ver[b].version;});
    std::iota(order.begin(),order.end(),usize{0});
    std::sort(order.begin(),order.end(),[&](usize a,usize b){return ver[a].object<ver[b].object;});
    usize reservation=absent;
    for(const auto i:order){
      const auto state=mga::InventoryVisibilityState(inventory[ver[i].creator_index]);
      if(state==mga::TransactionState::rolled_back||state==mga::TransactionState::failed_terminal)continue;
      if(reservation!=absent&&ver[i].object==ver[reservation].object&&ver[i].meta_row!=ver[reservation].meta_row)return E::duplicate_identity;
      reservation=i;
    }
    std::sort(order.begin(),order.end(),[&](usize a,usize b){
      return ver[a].row==ver[b].row?ver[a].row_version>ver[b].row_version:ver[a].row<ver[b].row;});
    const auto previous=[&](const VK& k)->usize{
      const auto found=std::lower_bound(by_version.begin(),by_version.end(),k.previous_version,[&](usize i,const Uuid& id){return ver[i].version<id;});
      return found!=by_version.end()&&ver[*found].version==k.previous_version?*found:absent;};
    for(usize at=0;at<n;++at){
      const auto i=order[at];const auto& k=ver[i];
      if(at&&ver[order[at-1]].row==k.row){const auto& prior=ver[order[at-1]];
        if(prior.row_version==k.row_version||prior.object!=k.object||prior.kind!=k.kind)return E::invalid_chain;}
      if(!k.previous_row_version)continue;
      if(const auto p=previous(k);p!=absent){
        if(ver[p].row!=k.row||ver[p].row_version!=k.previous_row_version)return E::invalid_chain;}
      // Even an absent declared UUID cannot disagree with an actually retained sequence.
      const auto seq=std::lower_bound(order.begin(),order.end(),i,[&](usize a,usize b){
        return ver[a].row==ver[b].row?ver[a].row_version>ver[b].previous_row_version:ver[a].row<ver[b].row;});
      if(seq!=order.end()&&ver[*seq].row==k.row&&ver[*seq].row_version==k.previous_row_version&&ver[*seq].version!=k.previous_version)
        return E::invalid_chain;
    }
    return E::none;
  }
  // SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1): at most one version per (row, creator transaction), whatever the creator's state.
  static bool OwnVersionRepeated(cohort_detail::Verifier& v,usize n) noexcept {
    auto ver=v.versions.first(n);auto order=v.order_b.first(n);
    std::iota(order.begin(),order.end(),usize{0});
    std::sort(order.begin(),order.end(),[&](usize a,usize b){
      return ver[a].row==ver[b].row?ver[a].creator_local<ver[b].creator_local:ver[a].row<ver[b].row;});
    for(usize at=1;at<n;++at)
      if(ver[order[at-1]].row==ver[order[at]].row&&ver[order[at-1]].creator_local==ver[order[at]].creator_local)return true;
    return false;
  }
  enum class PageIdentity {none,tree_twice,locator,duplicate};
  // A page seen twice. Index pages (the relation's index, or one tree's): by location then by page UUID. When the base
  // leaves are included: a base leaf that is also an index page is an invalid locator; two pages with one UUID are a duplicate.
  static PageIdentity PagesDistinct(cohort_detail::Verifier& v,usize bases,usize trees) noexcept {
    const auto total=bases+trees;
    auto order=v.page_order.first(total);
    std::iota(order.begin(),order.end(),usize{0});
    const auto is_tree=[&](usize i){return i>=bases;};
    const auto filespace=[&](usize i)->const Uuid&{return is_tree(i)?v.tree_pages[i-bases].filespace:v.pages[i].filespace;};
    const auto number=[&](usize i){return is_tree(i)?v.tree_pages[i-bases].page_number:v.pages[i].page_number;};
    const auto uuid=[&](usize i)->const Uuid&{return is_tree(i)?v.tree_pages[i-bases].page_uuid:v.pages[i].page_uuid;};
    std::sort(order.begin(),order.end(),[&](usize a,usize b){
      return filespace(a)==filespace(b)?number(a)<number(b):filespace(a)<filespace(b);});
    for(usize at=1;at<total;++at){const auto a=order[at-1],b=order[at];
      if(filespace(a)==filespace(b)&&number(a)==number(b))return is_tree(a)&&is_tree(b)?PageIdentity::tree_twice:PageIdentity::locator;}
    std::sort(order.begin(),order.end(),[&](usize a,usize b){return uuid(a)<uuid(b);});
    for(usize at=1;at<total;++at)if(uuid(order[at-1])==uuid(order[at]))
      return is_tree(order[at-1])&&is_tree(order[at])?PageIdentity::tree_twice:PageIdentity::duplicate;
    return PageIdentity::none;
  }
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
    // SB_NATIVE_CATALOG_CAPTURE_BOUND (Q219): resource_exhausted, naming the bound.
    const auto exhausted=[&](NativeCatalogCaptureBound bound){out.capture_bound=bound;return fail(E::resource_exhausted);};
    try {
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
      if(inventory.entries.size()>capture->transactions_.size())return exhausted(Bound::transactions);
      std::copy(inventory.entries.begin(),inventory.entries.end(),capture->transactions_.begin());
      capture->inventory_=NativeCapturedTransactionInventory{
          view.inventory_generation,
          {inventory.next_local_transaction_id,inventory.next_commit_sequence,
           capture->transactions_.first(inventory.entries.size())}};
    }
    using Stream=NativeCatalogVisibilityStreamReader;
    auto& verifier=workspace.verifier;
    for(usize catalog_index=0;catalog_index<roots.catalogs.size();++catalog_index){
      const u16 selector=catalog_index==roots.feature_root_index&&catalog_index!=0?8:2;
      const auto& root=roots.catalogs[catalog_index].root;
      if(capture)if(const auto b=capture->AppendPhysical(root.header,root.object_uuid);b!=Bound::none)return exhausted(b);
      ++out.needed_physical_pages;
      for(const auto& relation:root.roots){
        out.failing_selector=selector;out.failing_role=relation.role;
        if(out.relation_count==out.relations.size())return exhausted(Bound::relation_slots);
        if(out.verified_image_bytes>=maximum_image_bytes)return exhausted(Bound::image_bytes);
        const auto allowance=maximum_image_bytes-out.verified_image_bytes+source.retained_image_bytes();
        // ---- the pass: the visible rows to the capture, every stored version and page to the hooks ----
        const auto rows_before=capture?capture->count_:usize{0};
        usize visible=0;Bound visitor_bound=Bound::none;
        const auto visitor=[&](const NativeCatalogVisibleRow& row)->bool{
          ++visible;++out.needed_rows;out.needed_text_bytes+=row.metadata.record.payload.size();
          if(row.name_payload)if(const auto* entry=std::get_if<core::catalog::CatalogNameEntryView>(&*row.name_payload))
            out.needed_text_bytes+=entry->raw_name_text.size();
          if(capture)if(const auto b=capture->Append(selector,relation.role,row);b!=Bound::none){visitor_bound=b;return false;}
          return true;};
        Pass pass(verifier,capture,inventory.entries,allowance,workspace.navigation_pages,workspace.navigation_image_bytes);
        const auto streamed=Stream::StreamCommittedSelectedHooked(source,selector,relation.role,allowance,workspace.stream,visitor,pass);
        out.physical_bytes_read+=streamed.physical_bytes_read;
        if(streamed.stopped)return exhausted(visitor_bound);
        if(!streamed.ok()){
          using SE=NativeCatalogStreamError;using RE=NativeCatalogRelationLeaseError;using PE=NativePinnedCatalogReadError;
          const auto tree_exhausted=streamed.tree_error==page::NativeBtreeError::resource_exhausted;
          switch(streamed.error){
            case SE::hook_refused:{
              const auto& f=pass.failure();
              if(f.cohort==E::resource_exhausted)return exhausted(f.bound);
              if(f.cohort==E::unresolved_creator){out.unresolved_transaction_uuid=f.unresolved_transaction;
                out.unresolved_local_transaction_id=f.unresolved_local;return fail(E::unresolved_creator);}
              out.relation_error=f.relation;out.visibility_error=f.visibility;out.relation_tree_error=f.tree;
              return fail(E::relation_failure);}
            case SE::root_failure:out.relation_error=RE::root_failure;break;
            case SE::missing_relation:out.relation_error=RE::missing_relation;break;
            case SE::binding_mismatch:out.relation_error=RE::binding_mismatch;break;
            case SE::tree_failure:out.relation_error=RE::tree_failure;
              out.relation_tree_error=tree_exhausted?NativeBtreeTreeMemoryError::resource_exhausted:NativeBtreeTreeMemoryError::tree_failure;break;
            case SE::leaf_failure:out.relation_error=RE::leaf_failure;break;
            case SE::invalid_locator:out.relation_error=RE::invalid_locator;break;
            case SE::creator_mismatch:out.relation_error=RE::creator_mismatch;break;
            case SE::cluster_requires_authority:out.relation_error=RE::cluster_requires_authority;break;
            case SE::navigation_not_committed:out.visibility_error=PE::navigation_not_committed;break;
            case SE::invalid_chain:case SE::name_payload_invalid:out.visibility_error=PE::invalid_chain;break;
            case SE::missing_version:out.visibility_error=PE::missing_version;break;
            case SE::visibility_failure:out.visibility_error=PE::visibility_failure;break;
            case SE::requires_recovery:out.visibility_error=PE::requires_recovery;break;
            case SE::snapshot_failure:out.visibility_error=PE::snapshot_failure;break;
            case SE::io_failure:out.visibility_error=PE::io_failure;break;
            case SE::resource_exhausted:case SE::history_exhausted:
              out.relation_error=RE::resource_exhausted;
              if(tree_exhausted)out.relation_tree_error=NativeBtreeTreeMemoryError::resource_exhausted;break;
            case SE::memory_failure:case SE::invalid_request:case SE::none:return fail(E::invalid_request);
          }
          return fail(E::relation_failure);
        }
        // The verified-image account: this relation's roots, index pages and distinct base leaves (a leaf read again is not charged).
        {const auto additional=streamed.roots_retained_image_bytes+pass.charged_bytes();
          if(additional>maximum_image_bytes-out.verified_image_bytes)return exhausted(Bound::image_bytes);
          out.verified_image_bytes+=additional;}
        // The relation's identity: its first base leaf's (or an unindexed head's); an empty indexed relation with a universal
        // constant identity (the comments relation) has it without one.
        Uuid relation_uuid=streamed.relation_uuid;
        if(!core::uuid::IsEngineIdentityUuid(relation_uuid)&&streamed.indexed&&!pass.pages())
          if(const auto constant=NativeCatalogConstantRelationUuid(relation.role))relation_uuid=*constant;
        if(!core::uuid::IsEngineIdentityUuid(relation_uuid)){out.relation_error=NativeCatalogRelationLeaseError::missing_relation;return fail(E::relation_failure);}
        // ---- relation-wide checks: pages seen twice, then the sorted passes over the version keys ----
        switch(PagesDistinct(verifier,pass.pages(),pass.tree_pages())){
          case PageIdentity::none:break;
          case PageIdentity::tree_twice:out.relation_error=NativeCatalogRelationLeaseError::tree_failure;
            out.relation_tree_error=NativeBtreeTreeMemoryError::tree_failure;return fail(E::relation_failure);
          case PageIdentity::locator:out.relation_error=NativeCatalogRelationLeaseError::invalid_locator;return fail(E::relation_failure);
          case PageIdentity::duplicate:out.relation_error=NativeCatalogRelationLeaseError::duplicate_identity;return fail(E::relation_failure);
        }
        if(const auto wide=RelationWide(verifier,pass.versions(),inventory.entries);wide!=NativePinnedCatalogReadError::none){
          out.visibility_error=wide;return fail(E::relation_failure);}
        out.needed_physical_pages+=pass.pages()+pass.tree_pages();
        auto& inspection=out.relations[out.relation_count++];
        inspection={};
        inspection.selector=selector;inspection.role=relation.role;inspection.catalog_uuid=root.object_uuid;
        inspection.relation_uuid=relation_uuid;inspection.root=relation.page;
        inspection.catalog_generation=root.catalog_generation;inspection.schema_epoch=root.schema_epoch;
        inspection.security_epoch=root.security_epoch;inspection.resource_epoch=root.resource_epoch;
        inspection.base_pages=pass.pages();inspection.navigation_pages=pass.tree_pages();inspection.versions=pass.versions();
        inspection.visible_rows=visible;inspection.bindings=streamed.indexed?static_cast<usize>(streamed.cells):pass.versions();
        inspection.navigation_image_bytes=pass.tree_image_bytes();
        inspection.maximum_page_bytes=std::max<usize>(root.header.page_size_bytes,pass.max_page_bytes());
        if(capture){
          // The collect-everything reader emitted the visible rows by row UUID; so does the capture.
          std::sort(capture->rows_.begin()+static_cast<std::ptrdiff_t>(rows_before),capture->rows_.begin()+static_cast<std::ptrdiff_t>(capture->count_),
              [](const auto& a,const auto& b){return a.row.storage_row_uuid<b.row.storage_row_uuid;});
          if(const auto b=capture->AppendRelation({selector,relation.role,relation_uuid,
              streamed.indexed?pass.dependencies():std::optional<page::NativeBtreeDependencies>{}});b!=Bound::none)return exhausted(b);
        }
        const bool names_or_comments=selector==2&&(relation.role==2||relation.role==page::kNativeCommentRelationRole);
        if(names_or_comments&&OwnVersionRepeated(verifier,pass.versions()))return fail(E::own_version_repeated);
        const auto n=pass.versions();
        const auto find_version=[&](const Uuid& id)->usize{
          auto by=verifier.order_a.first(n);
          const auto found=std::lower_bound(by.begin(),by.end(),id,[&](usize i,const Uuid& u){return verifier.versions[i].version<u;});
          return found!=by.end()&&verifier.versions[*found].version==id?*found:std::numeric_limits<usize>::max();};
        // A tree cell binds to a row version of this relation: its base leaf, that version on it, the row's identity and creator.
        // `kind` 0 and 1 are the two name trees (an entry, for the authoritative one), 2 the comment tree.
        const auto walk_tree=[&](const auto& tree_root,unsigned kind)->bool{
          const bool comment=kind==2;const unsigned t=kind==1?1:0;
          const auto mismatch=comment?E::comment_tree_mismatch:E::name_tree_mismatch;
          const auto tree_failure=comment?E::comment_tree_failure:E::name_tree_failure;
          if(tree_root.page_type!=0x200){fail(tree_failure);return false;}
          if(out.verified_image_bytes>=maximum_image_bytes){exhausted(Bound::image_bytes);return false;}
          const auto limit=static_cast<usize>(std::min<u64>(maximum_image_bytes-out.verified_image_bytes,std::numeric_limits<usize>::max()));
          Pass tree_pass(verifier,capture,inventory.entries,limit,workspace.name_tree_pages,workspace.name_tree_image_bytes,false);
          std::fill_n(verifier.counters.begin(),n,u32{0});
          usize cells=0;Bound cell_bound=Bound::none;bool cell_bad=false;
          const auto on_cell=[&](const page::NativeBtreeCellView& cell)->bool{
            ++cells;
            const auto bad=[&]{cell_bad=true;return false;};
            if(!cell.base_page)return bad();
            const auto& ref=*cell.base_page;
            const auto page_index=tree_pass.FindPage(ref.filespace_uuid,ref.page_number);
            if(page_index==cohort_detail::kNone)return bad();
            const auto& page=verifier.pages[page_index];
            if(page.generation!=ref.page_generation||page.profile!=ref.page_size_profile_uuid)return bad();
            const auto at=find_version(cell.key.version_uuid);
            if(at==std::numeric_limits<usize>::max())return bad();
            const auto& k=verifier.versions[at];
            if(k.page!=page_index)return bad();
            if(comment){
              if(k.flags&VK::name)return bad();
              if(k.row!=cell.key.row_uuid||k.creator_local!=cell.creator_local_transaction_id||
                  k.kind!=static_cast<u16>(core::catalog::CatalogRecordKind::localized_comment)||!(k.flags&VK::comment_decoded))return bad();
              const auto key=core::catalog::CatalogCommentTreeKey(verifier.comment_keys[at].target,verifier.comment_keys[at].language);
              ++verifier.counters[at];
              if(cell.key.encoded_key.size()!=key.size()||!std::equal(key.begin(),key.end(),cell.key.encoded_key.begin())||!cell.payload.empty())return bad();
              return true;
            }
            if(!(k.flags&VK::name))return bad();
            if(k.row!=cell.key.row_uuid||k.creator_local!=cell.creator_local_transaction_id)return bad();
            if(t==0&&!(k.flags&VK::entry))return bad();
            ++verifier.counters[at];
            ++out.needed_name_tree_cells;out.needed_text_bytes+=cell.key.encoded_key.size()+cell.payload.size();
            if(capture)if(const auto b=capture->AppendNameTreeCell(static_cast<u16>(t),cell);b!=Bound::none){cell_bound=b;return false;}
            return true;
          };
          const auto walked=Stream::StreamTree(source,NativeCatalogStreamTreeOnly{tree_root.page,tree_root.object_uuid},limit,workspace.stream,on_cell,tree_pass);
          out.physical_bytes_read+=walked.physical_bytes_read;
          if(walked.stopped){
            if(cell_bound!=Bound::none)exhausted(cell_bound);else fail(mismatch);
            return false;}
          if(!walked.ok()){
            if(walked.error==NativeCatalogStreamError::hook_refused){
              const auto& f=tree_pass.failure();
              if(f.cohort==E::resource_exhausted&&f.bound!=Bound::none){exhausted(f.bound);return false;}
              out.name_tree_error=f.tree;out.name_tree_page_error=f.tree_page;
            }else{
              const bool exhausted_tree=walked.tree_error==page::NativeBtreeError::resource_exhausted||walked.error==NativeCatalogStreamError::resource_exhausted;
              out.name_tree_error=exhausted_tree?NativeBtreeTreeMemoryError::resource_exhausted:NativeBtreeTreeMemoryError::tree_failure;
              out.name_tree_page_error=walked.tree_error;
            }
            fail(tree_failure);return false;
          }
          if(tree_pass.charged_bytes()>maximum_image_bytes-out.verified_image_bytes){exhausted(Bound::image_bytes);return false;}
          out.verified_image_bytes+=tree_pass.charged_bytes();
          const auto& dependencies=tree_pass.dependencies();
          if(!dependencies||dependencies->index_uuid!=tree_root.object_uuid){fail(tree_failure);return false;}
          if(PagesDistinct(verifier,0,tree_pass.tree_pages())!=PageIdentity::none){
            out.name_tree_error=NativeBtreeTreeMemoryError::tree_failure;out.name_tree_page_error=page::NativeBtreeError::tree_reference_mismatch;
            fail(tree_failure);return false;}
          // Every row version has its cells: one in the authoritative tree per entry, one in the UUID-to-name tree per name row
          // (a deleted version none); a comment row version that is not a drop has exactly one. The cells were counted per row
          // version as they were bound, so this is one pass over the versions; with the cell count it is a bijection.
          usize expected=0;
          for(usize i=0;i<n;++i){
            const auto& k=verifier.versions[i];
            if(comment?!(k.flags&VK::comment):!(k.flags&VK::name))continue;
            // A drop's version (deleted) has no cells of its own: the version it retired keeps its stamped ones (owner Q42, N4a).
            const bool wants=!(k.flags&VK::deleted)&&(comment||t==1||(k.flags&VK::entry));
            const usize matches=verifier.counters[i];
            if(matches!=(wants?1u:0u)){
              // SB_NATIVE_NAME_CASCADE (N4b): a writer puts its base leaves before its tree pages, so a write that stopped between
              // them leaves versions without their cells. Their creator never committed: in progress when the server stopped
              // (dead once a writer restarts, owner Q23 = 1), rolled back or failed. Such a version may lack its cell (garbage a
              // sweep removes); any other creator's may not.
              bool admitted=false;
              if(wants&&matches==0){
                const auto state=transaction::mga::InventoryVisibilityState(inventory.entries[k.creator_index]);
                admitted=state==transaction::mga::TransactionState::active||state==transaction::mga::TransactionState::rolled_back||
                    state==transaction::mga::TransactionState::failed_terminal;
              }
              if(!admitted){fail(mismatch);return false;}
            }
            expected+=matches;
          }
          if(cells!=expected){fail(mismatch);return false;}
          out.needed_physical_pages+=tree_pass.tree_pages();
          if(comment){
            out.comment_tree_cells=cells;out.comment_tree_pages=tree_pass.tree_pages();
            if(capture)capture->comment_tree_=NativeCapturedNameTree{tree_root.kind,*dependencies,tree_root.page,cells,tree_pass.tree_pages()};
          }else{
            out.name_tree_cells[t]=cells;out.name_tree_pages[t]=tree_pass.tree_pages();
            if(capture)capture->name_trees_[t]=NativeCapturedNameTree{tree_root.kind,*dependencies,tree_root.page,cells,tree_pass.tree_pages()};
          }
          return true;
        };
        if(selector==2&&relation.role==2){
          // SB_NATIVE_NAME_TREES: the two name trees over this relation, read from page zero (kinds 22 and 23) while this
          // relation's version keys are live, so every cell can be bound to its row version.
          const auto* first=view.Root(22);const auto* second=view.Root(23);
          if((first!=nullptr)!=(second!=nullptr))return fail(E::name_tree_failure);
          if(first){
            if(!workspace.name_trees)return exhausted(Bound::tree_workspace);
            out.name_trees_present=true;
            for(unsigned t=0;t<2;++t)if(!walk_tree(t?*second:*first,t))return out;
          }
        }
        if(selector==2&&relation.role==page::kNativeCommentRelationRole){
          // SEARCH_KEY: SB_NATIVE_COMMENT_TREE (2026-11-13). The comments relation's (target, language) tree, read from page
          // zero (kind 24): every cell binds to a comment row version of this relation, its key is that row's target16 |
          // language16, and every comment row version that is not a drop has exactly one cell (the name trees' rule).
          if(const auto* comment_root=view.Root(24)){
            if(!workspace.name_trees)return exhausted(Bound::tree_workspace);
            out.comment_tree_present=true;
            if(!walk_tree(*comment_root,2))return out;
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
        if(capture->filespace_count_==capture->filespaces_.size())return exhausted(Bound::filespaces);
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
      // SB_OPEN_COHORT_STREAM: the pages were appended without the per-append search; a page twice (identical) is kept once,
      // a page twice with different identities is the conflict the search used to report.
      if(const auto b=capture->DedupePhysicalSorted();b!=Bound::none)return exhausted(b);
      if(!capture->initial_control_pages_.empty()){
        const auto append=[&](const auto& header,const auto& owner){return capture->AppendPhysicalTo(
            capture->initial_control_pages_,capture->initial_control_page_count_,header,owner,Bound::initial_control_pages);};
        if constexpr(fixed_root){
          // As created: page zero and its root set at generation one. Page
          // zero's roots are the root set; the open verifier reads them from
          // page zero itself, so no checkpoint roots are captured.
          const auto& id=source.identity();
          if(id.page_generation!=1||id.root_set_generation!=1)return fail(E::binding_mismatch);
          for(const auto& page:source.inventory_pages())
            if(const auto b=append(page.header,page.object_uuid);b!=Bound::none)return exhausted(b);
        }
        for(const auto& map:view.allocation->pages)
          if(const auto b=append(map.map.header,map.map.object_uuid);b!=Bound::none)return exhausted(b);
        for(const auto& page:view.directory->pages)
          if(const auto b=append(page.directory.header,page.directory.object_uuid);b!=Bound::none)return exhausted(b);
        auto controls=capture->initial_control_pages_.first(capture->initial_control_page_count_);
        std::sort(controls.begin(),controls.end(),[](const auto& a,const auto& b){return
            a.header.filespace_uuid<b.header.filespace_uuid||
            (a.header.filespace_uuid==b.header.filespace_uuid&&a.header.page_number<b.header.page_number);});
        capture->initial_controls_complete_=true;
      }
      capture->source_=out.source;
    }
    out.error=E::none;return out;
  }catch(...){return fail(E::invalid_request);}
  }
};
} // namespace scratchbird::storage::database
