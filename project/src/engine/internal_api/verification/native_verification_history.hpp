// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_verification_operation.hpp"
#include "native_startup_verification_request.hpp"
#include "native_management_history_regions.hpp"
#include "native_metadata_decode_scratch.hpp"
#include <map>

namespace scratchbird::engine::internal_api {
enum class NativeVerificationHistoryError {
  none,invalid_source,invalid_workspace,resource_exhausted,invalid_state,
  invalid_origin,binding_mismatch
};
// Validation of the registered SBVR state family within an already acquired
// management history. No policy/source authority, completed scan, recovery
// decision or full-verification receipt is inferred from retained declarations.
struct NativeVerificationHistoryObservation {
  NativeVerificationHistoryError error=NativeVerificationHistoryError::invalid_source;
  NativeVerificationStateError state_error=NativeVerificationStateError::none;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  Uuid failed_operation_uuid;
  u64 failed_revision=0,verified_operations=0,verified_revisions=0;
  std::size_t workspace_bytes=0;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok() const noexcept{return error==NativeVerificationHistoryError::none&&selection.has_value();}
};
namespace native_verification_history_detail {
inline bool Family(std::span<const byte> bytes) noexcept {
  return bytes.size()>=4&&std::equal(bytes.begin(),bytes.begin()+4,"SBVR");
}

}
inline NativeVerificationHistoryObservation VerifyNativeVerificationHistory(
    const storage::database::NativeManagementHistoryView& history,u64 maximum_record_bytes,
    std::span<byte> backing) noexcept {
  namespace db=storage::database;using E=NativeVerificationHistoryError;
  using namespace native_verification_history_detail;
  const auto fail=[](E error,const db::NativeManagementOperationView* record=nullptr,
      NativeVerificationStateError state=NativeVerificationStateError::none,
      db::NativeManagementOperationError operation=db::NativeManagementOperationError::none){
    NativeVerificationHistoryObservation out;out.error=error;out.state_error=state;out.operation_error=operation;
    if(record){out.failed_operation_uuid=record->uuid;out.failed_revision=record->revision;}return out;
  };
  if(!db::detail::DisjointNativeManagementHistoryBacking(history,backing))return fail(E::invalid_workspace);
  if(!history.ok()||!history.selection||!maximum_record_bytes)return fail(E::invalid_source);
  try {
    std::size_t files=0,objects=0,ranges=0,steps=0;
    for(const auto& entry:history.entries){const auto& record=entry.record;const auto state=record.operation_state_bytes;
      if(!Family(state))continue;
      if(!native_verification_state_detail::OperationFits(record,maximum_record_bytes))return fail(E::resource_exhausted,&record);
      const auto prefix=native_verification_state_detail::PrefixBytes(state);
      if(!prefix)return fail(E::invalid_state,&record,NativeVerificationStateError::invalid_header);
      const auto f=core::platform::LoadLittle32(state.data()+232),o=core::platform::LoadLittle32(state.data()+236),
          r=core::platform::LoadLittle32(state.data()+240),text=core::platform::LoadLittle32(state.data()+244);
      if(prefix+16*u64(f)+16*u64(o)+32*u64(r)+text!=state.size())return fail(E::invalid_state,&record,NativeVerificationStateError::invalid_header);
      files=std::max(files,std::size_t(f));objects=std::max(objects,std::size_t(o));ranges=std::max(ranges,std::size_t(r));steps=std::max(steps,record.steps.size());
    }
    if(steps>(std::numeric_limits<std::size_t>::max()-7)/2)return fail(E::resource_exhausted);
    db::detail::NativeMetadataMemory memory(backing);db::detail::NativeMetadataScratch scratch{&memory};
    // Each decoder owns independent fixed arrays; only boundary text borrows
    // the original immutable aggregate. Interleaved operations retain indexes,
    // never copies of records or mutable views into reused decoder scratch.
    NativeVerificationStateWorkspace a{scratch.Array<Uuid>(files),scratch.Array<Uuid>(objects),scratch.Array<NativeVerificationPageRange>(ranges)};
    NativeVerificationStateWorkspace b{scratch.Array<Uuid>(files),scratch.Array<Uuid>(objects),scratch.Array<NativeVerificationPageRange>(ranges)};
    const auto identities=scratch.Array<Uuid>(2*steps+7);
    std::pmr::map<Uuid,std::size_t> latest(&memory);
    u64 revisions=0;
    for(std::size_t n=0;n<history.entries.size();++n){const auto& record=history.entries[n].record;
      const auto prior=latest.find(record.uuid);
      if(prior==latest.end()&&!Family(record.operation_state_bytes))continue;
      if(record.database_uuid!=history.selection->header.database_uuid)return fail(E::binding_mismatch,&record);
      if(prior==latest.end()){
        const auto decoded=ReadNativeVerificationStateFromOperationInto(record,maximum_record_bytes,a,identities);
        if(!decoded.ok())return fail(decoded.error==NativeVerificationStateError::resource_exhausted?E::resource_exhausted:E::invalid_state,
            &record,decoded.error,decoded.operation_error);
        if(record.revision!=1||record.state!=db::NativeManagementState::created||
            decoded.record->phase!=NativeVerificationPhase::admitted||decoded.record->completion!=NativeVerificationCompletion::running)
          return fail(E::invalid_origin,&record);
        if(const auto& binding=history.entries[n].plan.intent.startup_binding;binding){
          const auto request=DecodeNativeStartupVerificationRequestInto(record.normalized_request_bytes,maximum_record_bytes,b);
          if(!request.ok()||request.request->binding!=*binding||binding->operation_uuid!=record.uuid||
              binding->session_uuid!=record.request_context_uuid||request.request->initial.acting_subject_uuid!=record.initiator_uuid||
              !native_verification_state_detail::Equal(request.request->initial,*decoded.record))
            return fail(E::binding_mismatch,&record,request.error);
        }
        latest.emplace(record.uuid,n);
      }else{
        const auto checked=ValidateNativeVerificationManagementEvolution(history.entries[prior->second].record,record,
            maximum_record_bytes,a,b,identities);
        if(!checked.ok())return fail(checked.error==NativeVerificationStateError::resource_exhausted?E::resource_exhausted:E::invalid_state,
            &record,checked.error,checked.operation_error);
        prior->second=n;
      }
      if(native_verification_state_detail::PrefixBytes(record.operation_state_bytes)>256){
        const auto decoded=ReadNativeVerificationStateFromOperationInto(record,maximum_record_bytes,a,identities);
        if(!decoded.ok()||!decoded.record->transaction_detail)return fail(E::invalid_state,&record,decoded.error);
        const auto& d=*decoded.record->transaction_detail;const auto& binding=history.entries[n].plan.intent.startup_binding;
        if(!binding||d.transaction_uuid!=binding->transaction_uuid||d.local_transaction_id!=binding->local_transaction_id||
            d.operation_uuid!=binding->operation_uuid||d.session_uuid!=binding->session_uuid)
          return fail(E::binding_mismatch,&record);
      }
      ++revisions;
    }
    NativeVerificationHistoryObservation out;out.error=E::none;out.selection=history.selection;
    out.verified_operations=latest.size();out.verified_revisions=revisions;out.workspace_bytes=memory.used();return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}
   catch(const std::length_error&){return fail(E::resource_exhausted);}
}
} // namespace scratchbird::engine::internal_api
