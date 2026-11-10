// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_name_path_verification.hpp"
#include "catalog_name_tree_keys.hpp"
#include "catalog_native_catalog_index.hpp"
#include "transaction_inventory.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeCatalogNameTreeVerificationError {
  none,source_required,workspace_required,cells_not_captured,duplicate_language_default,
  cell_without_row,cell_creator,cell_superseded,cell_row_kind,cell_key,cell_payload,row_without_cell
};
struct NativeCatalogNameTreeVerification {
  NativeCatalogNameTreeVerificationError error=NativeCatalogNameTreeVerificationError::source_required;
  // Whether page zero carried the trees (N2b-3 will require them of every
  // catalog-bearing filespace; until then a database without them verifies).
  bool present=false;
  core::platform::Uuid version_uuid;  // the cell's or row's version the finding is about
  std::size_t cells=0,rows=0;
  bool ok() const noexcept{return error==NativeCatalogNameTreeVerificationError::none;}
};
// SEARCH_KEY: SB_NATIVE_NAME_TREES. One semantic component of startup
// verification: every captured name tree cell against the captured catalog.
// The storage reader already bound each cell to a row version of the names
// relation by page, version, row and creator; this compares what the cell says
// with what the row says, allocation-free (core's *Equals functions):
//  - a cell whose version is a captured (visible) name row: the row's creator
//    is the cell's, the cell is not superseded (successors come with N4), the
//    row is an entry for the authoritative tree, and the key and payload are
//    what catalog_name_tree_keys derives from the row (the object's record
//    kind from its captured definition; an entry's path components from the
//    path rule over the captured names, SB_NATIVE_NAME_PATH);
//  - a cell whose version is not captured belongs to a version the snapshot
//    does not see: its creator must not be committed (a rolled-back or still
//    active insert; MGA's back versions, which a sweep removes); a committed
//    creator with no captured row is a dangling cell;
//  - every captured name row has its cells: one in the authoritative tree per
//    entry, one in the UUID-to-name tree per name row (counted in `colors`).
// Cells are found by scanning the captured cells per row (rows x cells, a few
// milliseconds on the default pack; an index is the page manager rebuild's).
inline NativeCatalogNameTreeVerification VerifyNativeCatalogNameTrees(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  namespace c=core::catalog;namespace ranges=storage::disk::detail;
  using E=NativeCatalogNameTreeVerificationError;
  NativeCatalogNameTreeVerification out;
  const auto fail=[&](E error,core::platform::Uuid version={}){out.error=error;out.version_uuid=version;out.cells=out.rows=0;return out;};
  if(!capture.ok())return out;
  const auto& trees=capture.name_trees();
  if(!trees[0]&&!trees[1]){out.error=E::none;return out;}
  out.present=true;
  if(!capture.has_name_tree_cells())return fail(E::cells_not_captured);
  const auto rows=capture.rows();
  if(workspace.colors.size()<rows.size()||!capture.DisjointWorkspace(std::as_bytes(workspace.colors))||
      !ranges::DisjointNativeDecodeRegions(workspace.order,workspace.parents,workspace.colors))return fail(E::workspace_required);
  NativeCatalogNamePathLookup lookup;bool workspace_short=false;core::platform::Uuid duplicate;
  if(!NativeCatalogNamePathLookup::Build(capture,workspace,lookup,workspace_short,duplicate))
    return workspace_short?fail(E::workspace_required):fail(E::duplicate_language_default,duplicate);
  auto counts=workspace.colors.first(rows.size());
  std::fill(counts.begin(),counts.end(),core::platform::byte{0});
  const auto inventory=capture.inventory();
  const auto creator_state=[&](core::platform::u64 local)->std::optional<transaction::mga::TransactionState>{
    if(!inventory)return std::nullopt;
    const auto& entries=inventory->inventory.entries;
    const auto at=std::lower_bound(entries.begin(),entries.end(),local,[](const auto& e,core::platform::u64 n){return e.identity.local_id.value<n;});
    if(at==entries.end()||at->identity.local_id.value!=local)return std::nullopt;
    return transaction::mga::InventoryVisibilityState(*at);
  };
  for(const auto& cell:capture.name_tree_cells()){
    ++out.cells;
    std::size_t found=rows.size();
    for(std::size_t i=0;i<rows.size()&&found==rows.size();++i)if(rows[i].row.version_uuid==cell.version_uuid)found=i;
    if(found==rows.size()){
      // Not a version this snapshot sees: its creator must not have committed.
      const auto state=creator_state(cell.creator_local_transaction_id);
      if(!state||*state==transaction::mga::TransactionState::committed)return fail(E::cell_without_row,cell.version_uuid);
      continue;
    }
    const auto& row=rows[found].row;
    if(rows[found].role!=2||!row.name_payload)return fail(E::cell_row_kind,cell.version_uuid);
    if(row.metadata.creator_local_transaction_id!=cell.creator_local_transaction_id)return fail(E::cell_creator,cell.version_uuid);
    if(cell.superseding_local_transaction_id)return fail(E::cell_superseded,cell.version_uuid);
    if(const auto* v=std::get_if<c::CatalogNameVectorView>(&*row.name_payload)){
      if(cell.tree!=1)return fail(E::cell_row_kind,cell.version_uuid);
      const auto* object=lookup.Find(v->object_uuid.value);
      if(!object||object->name_payload)return fail(E::cell_row_kind,cell.version_uuid);
      if(!c::CatalogNameUuidTreeVectorKeyEquals(v->object_uuid.value,object->metadata.record.header.kind,cell.key))
        return fail(E::cell_key,cell.version_uuid);
      if(!c::CatalogNameUuidTreeVectorPayloadEquals(*v,cell.payload))return fail(E::cell_payload,cell.version_uuid);
      counts[found]=static_cast<core::platform::byte>(counts[found]+16);
      continue;
    }
    const auto& e=std::get<c::CatalogNameEntryView>(*row.name_payload);
    const auto* object=lookup.Find(e.object_uuid.value);
    if(!object||object->name_payload)return fail(E::cell_row_kind,cell.version_uuid);
    const auto kind=object->metadata.record.header.kind;
    if(cell.tree==0){
      if(!c::CatalogNameAuthoritativeTreeKeyEquals(e,kind,cell.key))return fail(E::cell_key,cell.version_uuid);
      if(!c::CatalogNameAuthoritativeTreePayloadEquals(e,cell.payload))return fail(E::cell_payload,cell.version_uuid);
      counts[found]=static_cast<core::platform::byte>(counts[found]+1);
      continue;
    }
    if(!c::CatalogNameUuidTreeEntryKeyEquals(e,kind,cell.key))return fail(E::cell_key,cell.version_uuid);
    // The payload: the entry and its path's display components, root side
    // first; the walk gives them entry first, so compare from the end.
    std::array<std::string_view,c::kCatalogNamePathMaximumDepth> components;
    core::platform::Uuid entry_uuid;std::size_t count=0;
    if(!c::DecodeCatalogNameUuidTreeEntryPayloadInto(cell.payload,entry_uuid,components,count)||entry_uuid!=e.name_entry_uuid.value)
      return fail(E::cell_payload,cell.version_uuid);
    std::size_t walked=0;bool same=true;
    const auto error=c::WalkCatalogNamePath(e,lookup,[&](const c::CatalogNameEntryView& component){
      if(walked>=count||components[count-1-walked]!=component.display_name)same=false;
      ++walked;});
    if(error!=c::CatalogNamePathError::none||!same||walked!=count)return fail(E::cell_payload,cell.version_uuid);
    counts[found]=static_cast<core::platform::byte>(counts[found]+16);
  }
  for(std::size_t i=0;i<rows.size();++i){
    const auto& row=rows[i].row;
    if(rows[i].role!=2||!row.name_payload||row.metadata.record.header.deleted)continue;
    ++out.rows;
    const bool entry=std::holds_alternative<c::CatalogNameEntryView>(*row.name_payload);
    const unsigned authoritative=static_cast<unsigned>(counts[i])&15u,uuid=static_cast<unsigned>(counts[i])>>4;
    if(authoritative!=(entry?1u:0u)||uuid!=1u)return fail(E::row_without_cell,row.version_uuid);
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
