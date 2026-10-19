// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_policy_activation_state.hpp"
#include "native_management_history_regions.hpp"
#include "native_metadata_decode_scratch.hpp"
#include <map>

namespace scratchbird::engine::internal_api {
enum class NativePolicyActivationHistoryError {
  none,invalid_source,invalid_workspace,resource_exhausted,invalid_state,invalid_origin,binding_mismatch,invalid_predecessor
};
// Complete family history validation only. Actual MGA outcome, descriptor/body
// resolution, durable evidence and the boundary owner still control activation.
struct NativePolicyActivationHistoryObservation {
  NativePolicyActivationHistoryError error=NativePolicyActivationHistoryError::invalid_source;
  NativePolicyActivationStateError state_error=NativePolicyActivationStateError::none;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  core::platform::Uuid failed_operation_uuid;
  core::platform::u64 failed_revision=0,verified_operations=0,verified_revisions=0;
  std::size_t workspace_bytes=0;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok()const noexcept{return error==NativePolicyActivationHistoryError::none&&selection.has_value();}
};
inline NativePolicyActivationHistoryObservation VerifyNativePolicyActivationHistory(
    const storage::database::NativeManagementHistoryView& history,core::platform::u64 maximum_record_bytes,
    std::span<core::platform::byte> backing) noexcept {
  namespace db=storage::database;using E=NativePolicyActivationHistoryError;
  using Uuid=core::platform::Uuid;
  const auto family=[](auto bytes){return bytes.size()>=4&&std::equal(bytes.begin(),bytes.begin()+4,"SBPA");};
  const auto fail=[](E error,const db::NativeManagementOperationView* operation=nullptr,
      NativePolicyActivationStateError state=NativePolicyActivationStateError::none,
      db::NativeManagementOperationError common=db::NativeManagementOperationError::none){
    NativePolicyActivationHistoryObservation out;out.error=error;out.state_error=state;out.operation_error=common;
    if(operation){out.failed_operation_uuid=operation->uuid;out.failed_revision=operation->revision;}return out;
  };
  if(!db::detail::DisjointNativeManagementHistoryBacking(history,backing))return fail(E::invalid_workspace);
  if(!history.ok()||!history.selection||!maximum_record_bytes)return fail(E::invalid_source);
  try {
    std::size_t steps=0;
    for(const auto& entry:history.entries){const auto& record=entry.record;
      if(!family(record.operation_state_bytes))continue;
      if(!native_policy_activation_detail::Fits(record,maximum_record_bytes))return fail(E::resource_exhausted,&record);
      steps=std::max(steps,record.steps.size());
    }
    if(steps>(std::numeric_limits<std::size_t>::max()-7)/2)return fail(E::resource_exhausted);
    db::detail::NativeMetadataMemory memory(backing);db::detail::NativeMetadataScratch scratch{&memory};
    const auto identities=scratch.Array<Uuid>(2*steps+7);
    std::pmr::map<Uuid,std::size_t> latest(&memory);
    core::platform::u64 revisions=0;
    for(std::size_t n=0;n<history.entries.size();++n){const auto& record=history.entries[n].record;
      const auto prior=latest.find(record.uuid);
      if(prior==latest.end()&&!family(record.operation_state_bytes))continue;
      if(record.database_uuid!=history.selection->header.database_uuid)return fail(E::binding_mismatch,&record);
      if(prior==latest.end()){
        const auto decoded=ReadNativePolicyActivationStateFromOperationInto(record,maximum_record_bytes,identities);
        if(!decoded.ok())return fail(decoded.error==NativePolicyActivationStateError::resource_exhausted?E::resource_exhausted:E::invalid_state,
            &record,decoded.error,decoded.operation_error);
        if(record.revision!=1||record.state!=db::NativeManagementState::created)return fail(E::invalid_origin,&record);
        const auto& state=*decoded.record;
        if(!state.predecessor_uuid.is_nil()){
          const auto predecessor=latest.find(state.predecessor_uuid);
          if(predecessor==latest.end())return fail(E::invalid_predecessor,&record,NativePolicyActivationStateError::invalid_predecessor);
          const auto earlier=DecodeNativePolicyActivationState(history.entries[predecessor->second].record.operation_state_bytes,maximum_record_bytes);
          if(!earlier.ok())return fail(E::invalid_state,&record,earlier.error);
          const auto linked=ValidateNativePolicyActivationSuccessor(*earlier.record,state);
          if(linked!=NativePolicyActivationStateError::none)return fail(E::invalid_predecessor,&record,linked);
        }
        latest.emplace(record.uuid,n);
      }else{
        const auto checked=ValidateNativePolicyActivationManagementEvolution(history.entries[prior->second].record,record,maximum_record_bytes,identities);
        if(!checked.ok())return fail(checked.error==NativePolicyActivationStateError::resource_exhausted?E::resource_exhausted:E::invalid_state,
            &record,checked.error,checked.operation_error);
        prior->second=n;
      }
      ++revisions;
    }
    NativePolicyActivationHistoryObservation out;out.error=E::none;out.selection=history.selection;
    out.verified_operations=latest.size();out.verified_revisions=revisions;out.workspace_bytes=memory.used();return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
}
} // namespace scratchbird::engine::internal_api
