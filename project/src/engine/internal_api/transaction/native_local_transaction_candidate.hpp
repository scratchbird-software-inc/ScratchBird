// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include "native_decoded_storage_ranges.hpp"
#include <algorithm>
#include <limits>

namespace scratchbird::engine::internal_api {
enum class NativeLocalTransactionCandidateError {
  none, invalid_request, invalid_workspace, resource_exhausted, invalid_inventory,
  counter_exhausted, identity_mismatch, state_mismatch, cluster_requires_authority
};
enum class NativeLocalTransactionChange {
  allocate_starting, activate, activate_read_only, mark_rollback_only, start_rollback
};
struct NativeLocalTransactionCandidateRequest {
  NativeLocalTransactionChange change=NativeLocalTransactionChange::allocate_starting;
  core::platform::TypedUuid transaction_uuid;
  core::platform::u64 local_transaction_id=0;
  // Only allocation accepts these fields. The owning service supplies an
  // admitted clock observation and its selected snapshot mode.
  core::platform::u64 begin_unix_epoch_millis=0;
  bool stable_snapshot=false;
};
struct NativeLocalTransactionCandidateWorkspace {
  std::span<transaction::mga::TransactionInventoryEntry> entries;
  std::span<std::size_t> indices;
  std::span<core::platform::byte> markers;
};
struct NativeLocalTransactionCandidate {
  NativeLocalTransactionCandidateError error=NativeLocalTransactionCandidateError::invalid_request;
  storage::page::NativeTransactionInventoryConstView inventory;
  std::size_t changed_index=0;
  const char* detail="";
  bool ok()const noexcept{return error==NativeLocalTransactionCandidateError::none;}
};

// A bounded pure candidate, never BEGIN authority or a publication receipt.
// The owner retains the actual selected source, admission, issuer, and original
// publication across failures. Allocation publishes created separately from
// activation. No path here manufactures evidence or a final transaction outcome.
// Refusals leave the entry output untouched; scratch indices/markers may change.
inline NativeLocalTransactionCandidate BuildNativeLocalTransactionCandidate(
    const storage::page::NativeTransactionInventoryConstView& source,
    const NativeLocalTransactionCandidateRequest& request,
    NativeLocalTransactionCandidateWorkspace workspace) noexcept {
  namespace mga=transaction::mga;namespace page=storage::page;namespace disk=storage::disk;
  using E=NativeLocalTransactionCandidateError;using C=NativeLocalTransactionChange;
  using S=mga::TransactionState;using core::platform::u64;
  const auto fail=[](E error,const char* detail=""){NativeLocalTransactionCandidate r;r.error=error;r.detail=detail;return r;};
  const auto disjoint=[&](auto region){return disk::detail::DisjointNativeDecodeRegions(
      region,workspace.entries,workspace.indices,workspace.markers);};
  if(reinterpret_cast<std::uintptr_t>(workspace.entries.data())%alignof(mga::TransactionInventoryEntry)||
      reinterpret_cast<std::uintptr_t>(workspace.indices.data())%alignof(std::size_t)||
      !disjoint(std::span{&source,1})||!disjoint(std::span{&request,1})||
      !disjoint(std::span{&workspace,1})||!disjoint(source.entries))return fail(E::invalid_workspace);
  if(request.transaction_uuid.kind!=core::platform::UuidKind::transaction||
      !core::uuid::IsEngineIdentityUuid(request.transaction_uuid.value)||!request.local_transaction_id)
    return fail(E::invalid_request);
  const bool allocate=request.change==C::allocate_starting;
  switch(request.change){
    case C::allocate_starting:case C::activate:case C::activate_read_only:
    case C::mark_rollback_only:case C::start_rollback:break;
    default:return fail(E::invalid_request);
  }
  if(!allocate&&(request.begin_unix_epoch_millis||request.stable_snapshot))return fail(E::invalid_request);
  if(source.entries.size()==std::numeric_limits<std::size_t>::max()||
      workspace.entries.size()<source.entries.size()+std::size_t(allocate)||
      workspace.indices.size()<source.entries.size()||workspace.markers.size()<source.entries.size())
    return fail(E::resource_exhausted);
  const auto validated=page::ValidateNativeTransactionInventoryConstView(source,workspace.indices,workspace.markers);
  if(!validated.ok())return fail(validated.error==page::NativeInventoryError::invalid_backing?
      E::invalid_workspace:E::invalid_inventory,validated.detail);
  std::size_t selected=source.entries.size();
  u64 previous=0;
  for(std::size_t i=0;i<source.entries.size();++i){const auto& entry=source.entries[i];
    if(entry.identity.local_id.value<=previous)return fail(E::invalid_inventory,"inventory_order");
    previous=entry.identity.local_id.value;
    const bool same_local=entry.identity.local_id.value==request.local_transaction_id;
    const bool same_uuid=entry.identity.transaction_uuid.value==request.transaction_uuid.value;
    if(same_local!=same_uuid||allocate&&(same_local||same_uuid))return fail(E::identity_mismatch);
    if(same_local)selected=i;
  }
  mga::TransactionInventoryEntry changed;
  if(allocate){
    if(source.next_local_transaction_id==std::numeric_limits<u64>::max())return fail(E::counter_exhausted);
    if(request.local_transaction_id!=source.next_local_transaction_id)return fail(E::identity_mismatch);
    changed.identity={{request.local_transaction_id},request.transaction_uuid,mga::TransactionScope::local_node};
    changed.state=S::created;changed.begin_unix_epoch_millis=request.begin_unix_epoch_millis;
    changed.begin_visible_through_local_transaction_id=request.local_transaction_id-1;
    changed.begin_visible_through_commit_sequence=source.next_commit_sequence-1;
    changed.stable_snapshot=request.stable_snapshot;
  }else{
    if(selected==source.entries.size())return fail(E::identity_mismatch);
    changed=source.entries[selected];
    if(changed.identity.scope!=mga::TransactionScope::local_node)return fail(E::cluster_requires_authority);
    if(request.change==C::activate||request.change==C::activate_read_only){
      if(changed.state!=S::created||changed.rollback_only)return fail(E::state_mismatch);
      changed.state=request.change==C::activate?S::active:S::read_only_active;
    }else if(request.change==C::start_rollback){
      if(changed.state!=S::created&&changed.state!=S::active&&changed.state!=S::read_only_active&&
          changed.state!=S::preparing&&changed.state!=S::prepared)return fail(E::state_mismatch);
      changed.state=S::rolling_back;changed.rollback_only=true;
    }else{
      if(changed.state!=S::created&&changed.state!=S::active&&changed.state!=S::read_only_active&&
          changed.state!=S::preparing&&changed.state!=S::prepared)return fail(E::state_mismatch);
      changed.rollback_only=true;
    }
  }
  // No failing operation follows the first output write.
  std::copy(source.entries.begin(),source.entries.end(),workspace.entries.begin());
  workspace.entries[selected]=changed;
  return {E::none,{source.next_local_transaction_id+u64(allocate),source.next_commit_sequence,
      workspace.entries.first(source.entries.size()+std::size_t(allocate))},selected,""};
}
} // namespace scratchbird::engine::internal_api
