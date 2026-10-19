// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_verification_operation.hpp"
#include "native_publication_watermark.hpp"

namespace scratchbird::engine::internal_api {
// Exact first-startup intent: the registered binary64 startup tuple followed
// by the initial SBVR0001 state. Later phase changes affect operation_state,
// never these canonical request bytes. This representation grants no authority.
struct NativeStartupVerificationRequest {
  storage::database::NativeStartupBinding binding;
  NativeVerificationOperationState initial;
};
inline bool ValidNativeStartupVerificationRequest(const NativeStartupVerificationRequest& r) noexcept {
  const auto& b=r.binding;const auto& s=r.initial;
  return core::uuid::IsEngineIdentityUuid(b.operation_uuid)&&core::uuid::IsEngineIdentityUuid(b.session_uuid)&&
      core::uuid::IsEngineIdentityUuid(b.transaction_uuid)&&b.operation_uuid!=b.session_uuid&&
      b.operation_uuid!=b.transaction_uuid&&b.session_uuid!=b.transaction_uuid&&
      b.local_transaction_id>1&&b.local_transaction_id<UINT64_MAX&&b.fence_generation&&
      b.operation_uuid==s.operation_uuid&&s.phase==NativeVerificationPhase::admitted&&
      s.completion==NativeVerificationCompletion::running&&s.authority_class==NativeVerificationAuthority::startup_recovery&&
      (s.verification_class==NativeVerificationClass::startup_full||s.verification_class==NativeVerificationClass::startup_light)&&
      core::uuid::IsEngineIdentityUuid(s.acting_subject_uuid)&&s.snapshot_boundary_uuid.is_nil()&&
      s.maintenance_boundary_uuid.is_nil()&&s.cluster_uuid.is_nil()&&!s.cluster_epoch&&
      s.updated_at_micros==s.created_at_micros&&s.last_verified_boundary.empty()&&
      ValidateNativeVerificationOperationState(s)==NativeVerificationStateError::none;
}
struct NativeStartupVerificationRequestImage {
  NativeVerificationStateError error=NativeVerificationStateError::invalid_header;
  std::optional<NativeStartupVerificationRequest> request;
  std::span<const byte> bytes;
  bool ok()const noexcept{return error==NativeVerificationStateError::none&&request.has_value();}
};
inline NativeStartupVerificationRequestImage EncodeNativeStartupVerificationRequestInto(
    const NativeStartupVerificationRequest& r,u64 maximum_bytes,std::span<byte> output) noexcept {
  using E=NativeVerificationStateError;
  const auto safe=[&](auto region){return storage::disk::detail::DisjointNativeDecodeRegions(output,region);};
  if(!safe(std::span{&r,1})||!safe(r.initial.filespaces)||!safe(r.initial.objects)||!safe(r.initial.page_ranges)||
      !safe(std::span{r.initial.last_verified_boundary.data(),r.initial.last_verified_boundary.size()}))return {E::invalid_workspace};
  if(!ValidNativeStartupVerificationRequest(r))return {E::invalid_state};
  if(maximum_bytes<64||output.size()<64)return {E::resource_exhausted};
  auto state=EncodeNativeVerificationOperationStateInto(r.initial,maximum_bytes-64,output.subspan(64));
  if(!state.ok())return {state.error};
  const auto& b=r.binding;
  std::copy(b.operation_uuid.bytes.begin(),b.operation_uuid.bytes.end(),output.begin());
  std::copy(b.session_uuid.bytes.begin(),b.session_uuid.bytes.end(),output.begin()+16);
  std::copy(b.transaction_uuid.bytes.begin(),b.transaction_uuid.bytes.end(),output.begin()+32);
  core::platform::StoreLittle64(output.data()+48,b.local_transaction_id);
  core::platform::StoreLittle64(output.data()+56,b.fence_generation);
  return {E::none,r,output.first(64+state.bytes.size())};
}
inline NativeStartupVerificationRequestImage DecodeNativeStartupVerificationRequestInto(
    std::span<const byte> bytes,u64 maximum_bytes,NativeVerificationStateWorkspace scratch) noexcept {
  using E=NativeVerificationStateError;
  if(!storage::disk::detail::DisjointNativeDecodeRegions(bytes,scratch.filespaces,scratch.objects,scratch.page_ranges))
    return {E::invalid_workspace};
  if(maximum_bytes<64||bytes.size()>maximum_bytes)return {E::resource_exhausted};
  if(bytes.size()<64)return {E::invalid_header};
  auto decoded=DecodeNativeVerificationOperationStateInto(bytes.subspan(64),maximum_bytes-64,scratch);
  if(!decoded.ok())return {decoded.error};
  NativeStartupVerificationRequest r;r.initial=*decoded.record;
  std::copy_n(bytes.data(),16,r.binding.operation_uuid.bytes.begin());
  std::copy_n(bytes.data()+16,16,r.binding.session_uuid.bytes.begin());
  std::copy_n(bytes.data()+32,16,r.binding.transaction_uuid.bytes.begin());
  r.binding.local_transaction_id=core::platform::LoadLittle64(bytes.data()+48);
  r.binding.fence_generation=core::platform::LoadLittle64(bytes.data()+56);
  if(!ValidNativeStartupVerificationRequest(r))return {E::invalid_state};
  return {E::none,r,bytes};
}
} // namespace scratchbird::engine::internal_api
