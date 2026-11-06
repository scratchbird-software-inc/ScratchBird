// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "native_read_source_identity.hpp"

namespace scratchbird::storage::database {
struct NativeCatalogCaptureLimits {
  usize maximum_rows=0,maximum_text_bytes=0,maximum_filespaces=0;
  // Zero requests catalog/directory data only. A nonzero bound retains the
  // complete selected inventory; a partial transaction prefix is never exposed.
  usize maximum_transactions=0;
  // Optional complete physical catalog graph, including roots, navigation
  // and base leaves. Zero requests row/inventory capture only.
  usize maximum_physical_pages=0;
  // Optional complete initial checkpoint/map/inventory/directory identities.
  // Only an actual generation-one source can supply this creation graph.
  usize maximum_initial_control_pages=0;
};
struct NativeCapturedTransactionInventory {
  u64 generation=0;
  page::NativeTransactionInventoryConstView inventory;
};
struct NativeCapturedCatalogRow {
  u16 selector=0,role=0;
  NativeCatalogVisibleRow row;
};
// SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS: each captured relation's
// actual identity and, when indexed, the dependency tuple its index pages
// carry. Storage records them; the engine compares them with the catalog's
// profile and dependency-map rows.
struct NativeCapturedCatalogRelation {
  u16 selector=0,role=0;
  Uuid relation_uuid;
  std::optional<page::NativeBtreeDependencies> index_dependencies;
};
struct NativeCapturedCatalogPage {
  disk::NativeCommonPageHeader header;
  Uuid owner_uuid;
};
inline usize NativeCatalogCaptureWorkspaceBytes(NativeCatalogCaptureLimits limits) noexcept {
  // NativeStorageArena normalizes every allocation to max_align_t, including
  // the text block that follows the fixed row array.
  constexpr auto pad=7*(alignof(std::max_align_t)-1);
  if(!limits.maximum_rows||!limits.maximum_text_bytes||!limits.maximum_filespaces||limits.maximum_text_bytes>SIZE_MAX-pad||
      limits.maximum_rows>(SIZE_MAX-pad-limits.maximum_text_bytes)/sizeof(NativeCapturedCatalogRow))return 0;
  const auto rows=limits.maximum_rows*sizeof(NativeCapturedCatalogRow)+pad+limits.maximum_text_bytes;
  if(limits.maximum_filespaces>(SIZE_MAX-rows)/sizeof(page::NativeFilespaceDirectoryRecord))return 0;
  const auto filespaces=rows+limits.maximum_filespaces*sizeof(page::NativeFilespaceDirectoryRecord);
  if(limits.maximum_transactions>(SIZE_MAX-filespaces)/sizeof(transaction::mga::TransactionInventoryEntry))return 0;
  const auto inventory=filespaces+limits.maximum_transactions*sizeof(transaction::mga::TransactionInventoryEntry);
  if(limits.maximum_physical_pages>(SIZE_MAX-inventory)/sizeof(NativeCapturedCatalogPage))return 0;
  const auto physical=inventory+limits.maximum_physical_pages*sizeof(NativeCapturedCatalogPage);
  if(!limits.maximum_initial_control_pages)return physical;
  constexpr auto roots=32*sizeof(NativeCheckpointRootReference);
  if(physical>SIZE_MAX-roots||limits.maximum_initial_control_pages>(SIZE_MAX-physical-roots)/sizeof(NativeCapturedCatalogPage))return 0;
  return physical+roots+limits.maximum_initial_control_pages*sizeof(NativeCapturedCatalogPage);
}
class NativeCatalogCohortLeaseReader;
struct NativeCatalogCapturePreparedResult;
// Immutable selected rows for the engine's subsequent semantic verification.
// Every variable view is copied into admitted backing while the source is
// fenced. No source-page view survives reuse of the relation reader. This
// capture issues neither a verification receipt nor reference/permission proof.
class NativeCatalogCapture {
 public:
  NativeCatalogCapture() noexcept=default;
  NativeCatalogCapture(const NativeCatalogCapture&)=delete;
  NativeCatalogCapture& operator=(const NativeCatalogCapture&)=delete;
  NativeCatalogCapture(NativeCatalogCapture&& other) noexcept {Swap(other);}
  NativeCatalogCapture& operator=(NativeCatalogCapture&& other) noexcept {
    if(this!=&other){NativeCatalogCapture incoming(std::move(other));Swap(incoming);}return *this;
  }
  bool ok() const noexcept{return source_.has_value();}
  explicit operator bool() const noexcept{return bool(arena_);}
  std::span<const NativeCapturedCatalogRow> rows() const noexcept {
    return ok()?rows_.first(count_):std::span<const NativeCapturedCatalogRow>{};
  }
  const std::optional<NativeReadSourceIdentity>& source() const noexcept{return source_;}
  std::span<const page::NativeFilespaceDirectoryRecord> filespaces() const noexcept {
    return ok()?filespaces_.first(filespace_count_):std::span<const page::NativeFilespaceDirectoryRecord>{};
  }
  usize text_bytes() const noexcept{return ok()?used_:0;}
  bool has_physical_pages() const noexcept{return ok()&&!physical_pages_.empty();}
  std::span<const NativeCapturedCatalogPage> physical_pages() const noexcept {
    return ok()?physical_pages_.first(physical_page_count_):std::span<const NativeCapturedCatalogPage>{};
  }
  bool has_initial_controls() const noexcept{return ok()&&initial_controls_complete_;}
  std::span<const NativeCapturedCatalogPage> initial_control_pages() const noexcept {
    return has_initial_controls()?initial_control_pages_.first(initial_control_page_count_):std::span<const NativeCapturedCatalogPage>{};
  }
  std::span<const NativeCheckpointRootReference> initial_control_roots() const noexcept {
    return has_initial_controls()?initial_control_roots_.first(initial_control_root_count_):std::span<const NativeCheckpointRootReference>{};
  }
  std::optional<NativeCapturedTransactionInventory> inventory() const noexcept {
    return ok()?inventory_:std::nullopt;
  }
  std::span<const NativeCapturedCatalogRelation> relations() const noexcept {
    return ok()?std::span<const NativeCapturedCatalogRelation>{relations_}.first(relation_count_):std::span<const NativeCapturedCatalogRelation>{};
  }
  // Consumers may supply scratch storage, but it must not overwrite either
  // the fixed rows or their retained variable fields (including unused backing).
  bool DisjointWorkspace(std::span<const std::byte> workspace) const noexcept {
    return disk::detail::DisjointNativeDecodeRegions(workspace,rows_,text_,filespaces_,transactions_,physical_pages_,initial_control_pages_,initial_control_roots_,
        std::span<const NativeCatalogCapture>{this,1});
  }
 private:
  void Swap(NativeCatalogCapture& other) noexcept {
    using std::swap;swap(arena_,other.arena_);swap(rows_,other.rows_);swap(text_,other.text_);
    swap(count_,other.count_);swap(used_,other.used_);swap(source_,other.source_);
    swap(filespaces_,other.filespaces_);swap(filespace_count_,other.filespace_count_);
    swap(transactions_,other.transactions_);swap(inventory_,other.inventory_);
    swap(physical_pages_,other.physical_pages_);swap(physical_page_count_,other.physical_page_count_);
    swap(initial_control_pages_,other.initial_control_pages_);swap(initial_control_page_count_,other.initial_control_page_count_);
    swap(initial_control_roots_,other.initial_control_roots_);swap(initial_control_root_count_,other.initial_control_root_count_);
    swap(initial_controls_complete_,other.initial_controls_complete_);
    swap(relations_,other.relations_);swap(relation_count_,other.relation_count_);
  }
  void Begin() noexcept {source_.reset();inventory_.reset();count_=used_=filespace_count_=physical_page_count_=0;
    initial_control_page_count_=initial_control_root_count_=0;initial_controls_complete_=false;relation_count_=0;}
  bool AppendRelation(const NativeCapturedCatalogRelation& relation) noexcept {
    if(relation_count_==relations_.size())return false;
    relations_[relation_count_++]=relation;return true;
  }
  bool AppendPhysical(const disk::NativeCommonPageHeader& h,const Uuid& owner) noexcept {
    if(physical_pages_.empty())return true;
    return AppendPhysicalTo(physical_pages_,physical_page_count_,h,owner);
  }
  static bool AppendPhysicalTo(std::span<NativeCapturedCatalogPage> pages,usize& count,
      const disk::NativeCommonPageHeader& h,const Uuid& owner) noexcept {
    for(const auto& prior:pages.first(count)){
      const auto& p=prior.header;
      if(p.filespace_uuid!=h.filespace_uuid||p.page_number!=h.page_number)continue;
      return prior.owner_uuid==owner&&p.database_uuid==h.database_uuid&&p.page_size_profile_uuid==h.page_size_profile_uuid&&
          p.page_size_bytes==h.page_size_bytes&&p.page_type==h.page_type&&p.page_uuid==h.page_uuid&&
          p.page_generation==h.page_generation&&p.flags==h.flags;
    }
    if(count==pages.size())return false;
    pages[count++]={h,owner};return true;
  }
  bool Copy(std::string_view& value) noexcept {
    if(value.size()>text_.size()-used_)return false;
    if(value.empty()){value={};return true;}
    auto* target=text_.data()+used_;std::memcpy(target,value.data(),value.size());
    used_+=value.size();value={reinterpret_cast<const char*>(target),value.size()};return true;
  }
  bool Copy(std::span<const byte>& value) noexcept {
    if(value.size()>text_.size()-used_)return false;
    if(value.empty()){value={};return true;}
    auto target=text_.subspan(used_,value.size());std::memcpy(target.data(),value.data(),value.size());
    used_+=value.size();value=target;return true;
  }
  bool Append(u16 selector,u16 role,const NativeCatalogVisibleRow& source) noexcept {
    if(count_==rows_.size())return false;
    NativeCapturedCatalogRow value{selector,role,source};auto& metadata=value.row.metadata;
    if(!Copy(metadata.record.payload)||!Copy(metadata.trace_search_key)||!Copy(metadata.object_subtype)||
        !Copy(metadata.retention_class))return false;
    if(value.row.name_payload){
      if(auto* vector=std::get_if<core::catalog::CatalogNameVectorView>(&*value.row.name_payload)){
        if(!Copy(vector->object_class)||!Copy(vector->default_language_tag))return false;
      }else{
        auto& entry=std::get<core::catalog::CatalogNameEntryView>(*value.row.name_payload);
        if(!Copy(entry.object_class)||!Copy(entry.language_tag)||!Copy(entry.donor_id)||
            !Copy(entry.raw_name_text)||!Copy(entry.display_name)||!Copy(entry.normalized_lookup_key)||
            !Copy(entry.exact_lookup_key)||!Copy(entry.full_path_lookup_key))return false;
      }
    }
    rows_[count_++]=value;return true;
  }
  NativeStorageArena arena_;
  std::span<NativeCapturedCatalogRow> rows_;
  std::span<byte> text_;
  std::span<page::NativeFilespaceDirectoryRecord> filespaces_;
  std::span<transaction::mga::TransactionInventoryEntry> transactions_;
  std::span<NativeCapturedCatalogPage> physical_pages_;
  std::span<NativeCapturedCatalogPage> initial_control_pages_;
  std::span<NativeCheckpointRootReference> initial_control_roots_;
  usize initial_control_page_count_=0,initial_control_root_count_=0;
  bool initial_controls_complete_=false;
  std::optional<NativeCapturedTransactionInventory> inventory_;
  usize filespace_count_=0,physical_page_count_=0;
  usize count_=0,used_=0;
  std::optional<NativeReadSourceIdentity> source_;
  std::array<NativeCapturedCatalogRelation,12> relations_{};
  usize relation_count_=0;
  friend class NativeCatalogCohortLeaseReader;
  friend NativeCatalogCapturePreparedResult PrepareNativeCatalogCapture(
      NativeCatalogCaptureLimits,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeCatalogCapturePreparedResult {
  NativeStorageMemoryError error=NativeStorageMemoryError::invalid_binding;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeCatalogCapture capture;
  bool ok() const noexcept{return error==NativeStorageMemoryError::none&&bool(capture);}
};
inline NativeCatalogCapturePreparedResult PrepareNativeCatalogCapture(NativeCatalogCaptureLimits limits,
    NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  NativeCatalogCapturePreparedResult out;const auto bytes=NativeCatalogCaptureWorkspaceBytes(limits);
  if(!bytes)return out;
  auto allocation=memory.CreateArena(binding,bytes,alignof(NativeCapturedCatalogRow));
  out.error=allocation.error;out.allocation_status=allocation.backing.status;
  out.allocation_diagnostic=std::move(allocation.backing.diagnostic);
  if(!allocation.ok())return out;
  const auto rows=allocation.arena.Allocate(limits.maximum_rows*sizeof(NativeCapturedCatalogRow),alignof(NativeCapturedCatalogRow));
  const auto text=allocation.arena.Allocate(limits.maximum_text_bytes,1);
  const auto filespaces=allocation.arena.Allocate(limits.maximum_filespaces*sizeof(page::NativeFilespaceDirectoryRecord),
      alignof(page::NativeFilespaceDirectoryRecord));
  if(!rows.ok()||!text.ok()||!filespaces.ok()){out.error=NativeStorageMemoryError::resource_exhausted;return out;}
  static_assert(std::is_trivially_destructible_v<NativeCapturedCatalogRow>);
  auto* data=static_cast<NativeCapturedCatalogRow*>(rows.pointer);
  std::uninitialized_value_construct_n(data,limits.maximum_rows);
  static_assert(std::is_trivially_destructible_v<page::NativeFilespaceDirectoryRecord>);
  auto* members=static_cast<page::NativeFilespaceDirectoryRecord*>(filespaces.pointer);
  std::uninitialized_value_construct_n(members,limits.maximum_filespaces);
  out.capture.arena_=std::move(allocation.arena);out.capture.rows_={data,limits.maximum_rows};
  out.capture.filespaces_={members,limits.maximum_filespaces};
  if(limits.maximum_transactions){
    using Entry=transaction::mga::TransactionInventoryEntry;
    static_assert(std::is_trivially_destructible_v<Entry>);
    const auto entries=out.capture.arena_.Allocate(limits.maximum_transactions*sizeof(Entry),alignof(Entry));
    if(!entries.ok()){out.capture={};out.error=NativeStorageMemoryError::resource_exhausted;return out;}
    auto* transactions=static_cast<Entry*>(entries.pointer);
    std::uninitialized_value_construct_n(transactions,limits.maximum_transactions);
    out.capture.transactions_={transactions,limits.maximum_transactions};
  }
  if(limits.maximum_physical_pages){
    const auto pages=out.capture.arena_.Allocate(limits.maximum_physical_pages*sizeof(NativeCapturedCatalogPage),alignof(NativeCapturedCatalogPage));
    if(!pages.ok()){out.capture={};out.error=NativeStorageMemoryError::resource_exhausted;return out;}
    auto* values=static_cast<NativeCapturedCatalogPage*>(pages.pointer);
    std::uninitialized_value_construct_n(values,limits.maximum_physical_pages);
    out.capture.physical_pages_={values,limits.maximum_physical_pages};
  }
  if(limits.maximum_initial_control_pages){
    const auto pages=out.capture.arena_.Allocate(limits.maximum_initial_control_pages*sizeof(NativeCapturedCatalogPage),alignof(NativeCapturedCatalogPage));
    const auto roots=out.capture.arena_.Allocate(32*sizeof(NativeCheckpointRootReference),alignof(NativeCheckpointRootReference));
    if(!pages.ok()||!roots.ok()){out.capture={};out.error=NativeStorageMemoryError::resource_exhausted;return out;}
    auto* values=static_cast<NativeCapturedCatalogPage*>(pages.pointer);
    auto* references=static_cast<NativeCheckpointRootReference*>(roots.pointer);
    std::uninitialized_value_construct_n(values,limits.maximum_initial_control_pages);
    std::uninitialized_value_construct_n(references,32);
    out.capture.initial_control_pages_={values,limits.maximum_initial_control_pages};
    out.capture.initial_control_roots_={references,32};
  }
  out.capture.text_={static_cast<byte*>(text.pointer),text.bytes};out.error=NativeStorageMemoryError::none;return out;
}
} // namespace scratchbird::storage::database
