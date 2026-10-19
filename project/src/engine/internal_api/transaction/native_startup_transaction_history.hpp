// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_history_regions.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_publication_coordinator.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::engine::internal_api {
enum class NativeStartupTransactionHistoryError {
  none, invalid_source, source_mismatch, invalid_workspace, resource_exhausted,
  invalid_history, invalid_binding, invalid_inventory, identity_mismatch
};
enum class NativeStartupInventoryOutcome { missing, unresolved, committed, rolled_back, failed_terminal, cluster_owned };
struct NativeStartupTransactionObservation {
  storage::database::NativeStartupBinding binding;
  core::platform::u64 management_revision=0;
  storage::database::NativeManagementState management_state=storage::database::NativeManagementState::created;
  transaction::mga::TransactionState inventory_state=transaction::mga::TransactionState::none;
  NativeStartupInventoryOutcome outcome=NativeStartupInventoryOutcome::missing;
  core::platform::u64 commit_sequence=0;
};
struct NativeStartupTransactionHistoryObservation {
  NativeStartupTransactionHistoryError error=NativeStartupTransactionHistoryError::invalid_source;
  core::platform::Uuid failed_operation_uuid;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  std::span<const NativeStartupTransactionObservation> transactions;
  // An unselected reservation can exist before any corresponding inventory
  // entry or management extent. It must remain visible to owning recovery.
  std::optional<storage::database::NativeStartupBinding> pending_binding;
  core::platform::Uuid pending_publication_uuid;
  core::platform::u64 pending_publication_generation=0;
  std::size_t workspace_bytes=0;
  bool ok()const noexcept{return error==NativeStartupTransactionHistoryError::none&&selection.has_value();}
};
namespace native_startup_transaction_history_detail {
inline bool SameSelection(const storage::database::NativeCheckpointSelection& a,
    const storage::database::NativeCheckpointSelection& b)noexcept {
  return a.header.database_uuid==b.header.database_uuid&&a.header.filespace_uuid==b.header.filespace_uuid&&
    a.object_uuid==b.object_uuid&&a.bootstrap_uuid==b.bootstrap_uuid&&a.publication_uuid==b.publication_uuid&&
    a.selection_generation==b.selection_generation&&a.checkpoint==b.checkpoint&&a.checkpoint_object_uuid==b.checkpoint_object_uuid&&
    a.checkpoint_sha256==b.checkpoint_sha256&&a.checkpoint_generation==b.checkpoint_generation&&
    a.root_set_generation==b.root_set_generation&&a.timeline_uuid==b.timeline_uuid&&
    a.previous_selection_generation==b.previous_selection_generation&&a.previous_checkpoint==b.previous_checkpoint&&
    a.previous_checkpoint_object_uuid==b.previous_checkpoint_object_uuid&&a.previous_checkpoint_sha256==b.previous_checkpoint_sha256;
}
inline bool Binding(const storage::database::NativeStartupBinding& b)noexcept {
  return core::uuid::IsEngineIdentityUuid(b.operation_uuid)&&core::uuid::IsEngineIdentityUuid(b.session_uuid)&&
    core::uuid::IsEngineIdentityUuid(b.transaction_uuid)&&b.local_transaction_id&&b.local_transaction_id<UINT64_MAX&&b.fence_generation;
}
}

// Join actual selected management history, complete inventory and the observed
// publication watermark. The native host supplies all three under original
// custody. This read does not resolve an operation, certify evidence, clear a
// startup fence or permit a fresh attempt. Even committed/rolled-back entries
// need matching owning operation reconciliation; missing entries mean unknown.
// All returned rows borrow backing. No output view escapes any refusal.
inline NativeStartupTransactionHistoryObservation InspectNativeStartupTransactionHistory(
    const storage::database::NativeManagementHistoryView& history,
    const storage::database::NativeCheckpointSelection& inventory_selection,
    const storage::page::NativeTransactionInventoryConstView& inventory,
    const storage::database::NativePublicationSnapshot& publication,
    std::span<core::platform::byte> backing)noexcept {
  namespace db=storage::database;namespace page=storage::page;namespace mga=transaction::mga;
  using E=NativeStartupTransactionHistoryError;using O=NativeStartupInventoryOutcome;using S=mga::TransactionState;
  using core::platform::Uuid;using namespace native_startup_transaction_history_detail;
  const auto fail=[](E error,Uuid operation={}){NativeStartupTransactionHistoryObservation out;out.error=error;out.failed_operation_uuid=operation;return out;};
  if(!db::detail::DisjointNativeManagementHistoryBacking(history,backing)||
      !storage::disk::detail::DisjointNativeDecodeRegions(backing,std::span{&inventory_selection,1})||
      !storage::disk::detail::DisjointNativeDecodeRegions(backing,std::span{&inventory,1})||
      !storage::disk::detail::DisjointNativeDecodeRegions(backing,inventory.entries)||
      !storage::disk::detail::DisjointNativeDecodeRegions(backing,std::span{&publication,1}))return fail(E::invalid_workspace);
  if(!history.ok()||!history.selection)return fail(E::invalid_source);
  if(!SameSelection(*history.selection,inventory_selection)||!SameSelection(*history.selection,publication.selection))
    return fail(E::source_mismatch);
  const auto& selected=publication.selection;const auto& observed=publication.watermark;
  if(observed.header.database_uuid!=selected.header.database_uuid||observed.header.filespace_uuid!=selected.header.filespace_uuid||
      observed.bootstrap_uuid!=selected.bootstrap_uuid||observed.timeline_uuid!=selected.timeline_uuid||!observed.watermark)
    return fail(E::source_mismatch);
  try {
    db::detail::NativeMetadataMemory memory(backing);db::detail::NativeMetadataScratch scratch{&memory};
    const auto indices=scratch.Array<std::size_t>(inventory.entries.size());
    const auto uuid_indices=scratch.Array<std::size_t>(inventory.entries.size());
    const auto markers=scratch.Array<core::platform::byte>(inventory.entries.size());
    const auto valid=page::ValidateNativeTransactionInventoryConstView(inventory,indices,markers);
    if(!valid.ok())return fail(valid.error==page::NativeInventoryError::invalid_backing?E::invalid_workspace:E::invalid_inventory);
    const auto entries=inventory.entries;
    for(std::size_t i=0;i<entries.size();++i)indices[i]=uuid_indices[i]=i;
    if(entries.size()>1){
      std::sort(indices.begin(),indices.end(),[&](auto a,auto b){return entries[a].identity.local_id.value<entries[b].identity.local_id.value;});
      std::sort(uuid_indices.begin(),uuid_indices.end(),[&](auto a,auto b){return entries[a].identity.transaction_uuid.value<entries[b].identity.transaction_uuid.value;});
    }
    const auto observe=[&](const db::NativeStartupBinding& binding,NativeStartupTransactionObservation& result){
      if(!Binding(binding))return E::invalid_binding;
      const auto at=std::lower_bound(indices.begin(),indices.end(),binding.local_transaction_id,
          [&](auto i,auto local){return entries[i].identity.local_id.value<local;});
      const mga::TransactionInventoryEntry* found=at==indices.end()||entries[*at].identity.local_id.value!=binding.local_transaction_id?nullptr:&entries[*at];
      if(found&&found->identity.transaction_uuid.value!=binding.transaction_uuid)return E::identity_mismatch;
      if(!found){const auto by_uuid=std::lower_bound(uuid_indices.begin(),uuid_indices.end(),binding.transaction_uuid,
          [&](auto i,const Uuid& id){return entries[i].identity.transaction_uuid.value<id;});
        return by_uuid!=uuid_indices.end()&&entries[*by_uuid].identity.transaction_uuid.value==binding.transaction_uuid?E::identity_mismatch:E::none;}
      result.inventory_state=found->state;result.commit_sequence=found->commit_sequence;
      if(found->identity.scope!=mga::TransactionScope::local_node){result.outcome=O::cluster_owned;return E::none;}
      switch(mga::InventoryVisibilityState(*found)){
        case S::committed:result.outcome=O::committed;break;
        case S::rolled_back:result.outcome=O::rolled_back;break;
        case S::failed_terminal:result.outcome=O::failed_terminal;break;
        default:result.outcome=O::unresolved;break;
      }
      return E::none;
    };
    const auto latest=history.latest;
    std::size_t count=0;
    for(std::size_t i=0;i<latest.size();++i){const auto& key=latest[i];
      if(key.entry>=history.entries.size()||(i&&!(latest[i-1].operation_uuid<key.operation_uuid))||
          history.entries[key.entry].record.uuid!=key.operation_uuid)return fail(E::invalid_history,key.operation_uuid);
      if(history.entries[key.entry].plan.intent.startup_binding)++count;
    }
    for(std::size_t i=0;i<history.entries.size();++i){const auto& entry=history.entries[i];const auto& record=entry.record;
      const auto at=std::lower_bound(latest.begin(),latest.end(),record.uuid,[](const auto& key,const Uuid& id){return key.operation_uuid<id;});
      if(at==latest.end()||at->operation_uuid!=record.uuid||at->entry<i)return fail(E::invalid_history,record.uuid);
      const auto& binding=entry.plan.intent.startup_binding;
      if(binding!=history.entries[at->entry].plan.intent.startup_binding||record.database_uuid!=inventory_selection.header.database_uuid)
        return fail(E::invalid_history,record.uuid);
      if(binding&&(!Binding(*binding)||binding->operation_uuid!=record.uuid||record.normalized_request_bytes.empty()))return fail(E::invalid_binding,record.uuid);
    }
    const auto output=scratch.Array<NativeStartupTransactionObservation>(count);
    std::size_t written=0;
    for(const auto& key:latest){const auto& entry=history.entries[key.entry];
      if(!entry.plan.intent.startup_binding)continue;
      NativeStartupTransactionObservation result;result.binding=*entry.plan.intent.startup_binding;
      result.management_revision=entry.record.revision;result.management_state=entry.record.state;
      const auto error=observe(result.binding,result);if(error!=E::none)return fail(error,key.operation_uuid);
      output[written++]=result;
    }
    NativeStartupTransactionHistoryObservation result;
    const auto& watermark=publication.watermark;
    if(watermark.watermark<publication.selection.checkpoint_generation)return fail(E::source_mismatch);
    if(watermark.watermark>publication.selection.checkpoint_generation&&watermark.intent&&watermark.intent->startup_binding){
      const auto& binding=*watermark.intent->startup_binding;
      if(watermark.intent->recovery_profile!=2||watermark.abandonment||!core::uuid::IsEngineIdentityUuid(watermark.operation_uuid))
        return fail(E::invalid_binding,binding.operation_uuid);
      NativeStartupTransactionObservation pending;pending.binding=binding;
      const auto error=observe(binding,pending);if(error!=E::none)return fail(error,binding.operation_uuid);
      result.pending_binding=binding;result.pending_publication_uuid=watermark.operation_uuid;
      result.pending_publication_generation=watermark.watermark;
    }
    result.error=E::none;result.selection=history.selection;result.transactions=output;result.workspace_bytes=memory.used();return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
}
} // namespace scratchbird::engine::internal_api
