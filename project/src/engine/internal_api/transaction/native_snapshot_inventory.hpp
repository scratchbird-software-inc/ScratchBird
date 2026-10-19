// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "transaction/mga/transaction_horizon_projection.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeSnapshotInventoryError {
  none,invalid_request,invalid_workspace,resource_exhausted,invalid_inventory,
  owner_mismatch,owner_ineligible,cluster_requires_authority
};
struct NativeSnapshotInventoryWorkspace {
  std::span<core::platform::u64> active,in_doubt;
  std::span<std::size_t> indices;
  std::span<core::platform::byte> markers;
};
// An immutable bounded projection, not a registered snapshot, selected source,
// visibility grant, retention pin or BEGIN. The actual owner supplies all of
// those and retains these arrays throughout the published snapshot's lifetime.
struct NativeSnapshotInventoryProjection {
  NativeSnapshotInventoryError error=NativeSnapshotInventoryError::invalid_request;
  core::platform::u64 next_transaction_number=0,visible_committed_high_watermark=0;
  core::platform::u64 visible_commit_sequence=0,owning_transaction_number=0;
  core::platform::TypedUuid owning_transaction_uuid;
  transaction::mga::LocalTransactionHorizons horizons;
  core::platform::u64 retention_horizon=0;
  std::span<const core::platform::u64> active,in_doubt;
  bool ok()const noexcept{return error==NativeSnapshotInventoryError::none;}
};
// Build from a complete validated inventory under the owner's stable selection.
// A created owner is allowed only to prepare its snapshot before active
// admission; it remains in doubt and this projection cannot activate it.
// No heap fallback. Every refusal leaves both exclusion arrays unchanged.
inline NativeSnapshotInventoryProjection ProjectNativeSnapshotInventory(
    const storage::page::NativeTransactionInventoryConstView& source,
    const core::platform::TypedUuid& owner_uuid,core::platform::u64 owner_number,
    NativeSnapshotInventoryWorkspace workspace) noexcept {
  using E=NativeSnapshotInventoryError;using S=transaction::mga::TransactionState;
  using core::platform::u64;namespace mga=transaction::mga;
  const auto fail=[](E e){NativeSnapshotInventoryProjection r;r.error=e;return r;};
  const auto disjoint=[&](auto region){return storage::disk::detail::DisjointNativeDecodeRegions(
      region,workspace.active,workspace.in_doubt,workspace.indices,workspace.markers);};
  if(reinterpret_cast<std::uintptr_t>(workspace.active.data())%alignof(u64)||
      reinterpret_cast<std::uintptr_t>(workspace.in_doubt.data())%alignof(u64)||
      reinterpret_cast<std::uintptr_t>(workspace.indices.data())%alignof(std::size_t)||
      !disjoint(std::span{&source,1})||!disjoint(std::span{&owner_uuid,1})||
      !disjoint(std::span{&workspace,1})||!disjoint(source.entries))return fail(E::invalid_workspace);
  if(!owner_number||owner_uuid.kind!=core::platform::UuidKind::transaction||
      !core::uuid::IsEngineIdentityUuid(owner_uuid.value))return fail(E::invalid_request);
  if(workspace.indices.size()<source.entries.size()||workspace.markers.size()<source.entries.size())
    return fail(E::resource_exhausted);
  const auto valid=storage::page::ValidateNativeTransactionInventoryConstView(source,workspace.indices,workspace.markers);
  if(!valid.ok())return fail(valid.error==storage::page::NativeInventoryError::invalid_backing?
      E::invalid_workspace:E::invalid_inventory);
  const auto active=[](S s){return s==S::active||s==S::read_only_active;};
  const auto uncertain=[](S s){return s==S::created||s==S::preparing||s==S::prepared||
      s==S::committing||s==S::rolling_back||s==S::limbo||s==S::recovering;};
  std::size_t active_count=0,in_doubt_count=0;u64 previous=0;bool found=false;
  u64 retention=owner_number;
  for(const auto& entry:source.entries){const auto number=entry.identity.local_id.value;
    if(number<=previous)return fail(E::invalid_inventory);
    previous=number;
    if(entry.identity.scope!=mga::TransactionScope::local_node)return fail(E::cluster_requires_authority);
    const bool same_number=number==owner_number,same_uuid=entry.identity.transaction_uuid.kind==owner_uuid.kind&&
        entry.identity.transaction_uuid.value==owner_uuid.value;
    if(same_number!=same_uuid)return fail(E::owner_mismatch);
    if(same_number){
      if((entry.state!=S::created&&!active(entry.state))||entry.rollback_only)return fail(E::owner_ineligible);
      found=true;
    }
    if(active(entry.state))++active_count;
    if(uncertain(entry.state))++in_doubt_count;
    if(active(entry.state)||uncertain(entry.state))retention=std::min(retention,number);
  }
  if(!found)return fail(E::owner_mismatch);
  if(workspace.active.size()<active_count||workspace.in_doubt.size()<in_doubt_count)return fail(E::resource_exhausted);
  NativeSnapshotInventoryProjection result;
  // Calculate OIT/OAT from the actual inventory. The new snapshot participates
  // explicitly in OST; runtime retention is never inferred by a disk decoder.
  mga::LocalTransactionHorizons base;
  if(mga::detail::ProjectValidatedLocalHorizons(source,{},base))return fail(E::invalid_inventory);
  retention=std::min(retention,base.oldest_interesting_transaction.value);
  const mga::LocalTransactionId retained{retention};
  if(mga::detail::ProjectValidatedLocalHorizons(source,std::span{&retained,1},result.horizons))return fail(E::invalid_inventory);
  // No failure is possible after the first output write.
  std::size_t a=0,d=0;
  for(const auto& entry:source.entries){
    if(active(entry.state))workspace.active[a++]=entry.identity.local_id.value;
    if(uncertain(entry.state))workspace.in_doubt[d++]=entry.identity.local_id.value;
  }
  result.error=E::none;result.next_transaction_number=source.next_local_transaction_id;
  result.visible_committed_high_watermark=source.next_local_transaction_id-1;
  result.visible_commit_sequence=source.next_commit_sequence-1;
  result.owning_transaction_number=owner_number;result.owning_transaction_uuid=owner_uuid;
  result.retention_horizon=std::min(retention,result.horizons.oldest_snapshot_transaction.value);
  result.active=workspace.active.first(a);result.in_doubt=workspace.in_doubt.first(d);return result;
}
} // namespace scratchbird::engine::internal_api
