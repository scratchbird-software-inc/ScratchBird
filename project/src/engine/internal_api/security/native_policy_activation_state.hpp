// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_operation.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "catalog_policy_descriptor.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace scratchbird::engine::internal_api {
enum class NativePolicyActivationAction : core::platform::u16 { activate=1,revoke };
// Immutable intent nested in SBMGO003. A decoded intent, completed aggregate
// or descriptor match does not activate a policy or prove committed finality.
struct NativePolicyActivationState {
  core::platform::Uuid operation_uuid,database_uuid,policy_uuid,policy_version_uuid,body_uuid;
  core::platform::Uuid boundary_uuid,creating_transaction_uuid,predecessor_uuid,cluster_uuid;
  core::platform::u64 policy_generation=0,body_generation=0,sequence=0,creating_local_transaction_id=0;
  std::optional<core::platform::u64> cluster_epoch;
  NativePolicyActivationAction action=NativePolicyActivationAction::activate;
  core::catalog::CatalogPolicyActivation boundary=core::catalog::CatalogPolicyActivation::new_transaction;
  bool operator==(const NativePolicyActivationState&)const=default;
};
enum class NativePolicyActivationStateError {
  none,invalid_header,invalid_identity,invalid_state,invalid_predecessor,
  immutable_intent,binding_mismatch,resource_exhausted,invalid_workspace,operation_failure
};
struct NativePolicyActivationStateImage {
  NativePolicyActivationStateError error=NativePolicyActivationStateError::invalid_state;
  std::optional<NativePolicyActivationState> record;
  std::span<const core::platform::byte> bytes;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  bool ok()const noexcept{return error==NativePolicyActivationStateError::none&&record.has_value();}
};
namespace native_policy_activation_detail {
using namespace core::platform;
using Uuid=core::platform::Uuid;
using S=NativePolicyActivationState;using E=NativePolicyActivationStateError;
using A=NativePolicyActivationAction;using B=core::catalog::CatalogPolicyActivation;
using M=storage::database::NativeManagementOperationView;
inline constexpr std::array<Uuid S::*,9> identities{
  &S::operation_uuid,&S::database_uuid,&S::policy_uuid,&S::policy_version_uuid,&S::body_uuid,
  &S::boundary_uuid,&S::creating_transaction_uuid,&S::predecessor_uuid,&S::cluster_uuid};
template<class T>bool Region(std::span<T> bytes) noexcept {
  return storage::disk::detail::DecodedRange(bytes).valid&&
      (bytes.empty()||reinterpret_cast<std::uintptr_t>(bytes.data())%alignof(T)==0);
}
inline bool SameTuple(const S& a,const S& b) noexcept {
  return a.policy_generation==b.policy_generation&&a.policy_version_uuid==b.policy_version_uuid&&
    a.body_uuid==b.body_uuid&&a.body_generation==b.body_generation;
}
inline bool Disjoint(const M& operation,std::span<Uuid> scratch) noexcept {
  const auto safe=[&](auto region){return storage::disk::detail::DisjointNativeDecodeRegions(scratch,region);};
  if(!Region(operation.steps)||!Region(scratch)||!safe(std::span{&operation,1})||!safe(operation.steps)||
    !safe(operation.normalized_request_bytes)||!safe(operation.operation_state_bytes)||
    !safe(std::span{operation.idempotency_key.data(),operation.idempotency_key.size()}))return false;
  for(const auto& step:operation.steps)
    if(!safe(std::span{step.idempotency_key.data(),step.idempotency_key.size()}))return false;
  return true;
}
inline bool Fits(const M& operation,u64 budget) noexcept {
  const auto ceiling=std::min<u64>(budget,UINT32_MAX);u64 used=0;
  const auto charge=[&](std::size_t n,u64 width=1){
    if(used>ceiling||n>(ceiling-used)/width)return false;used+=n*width;return true;};
  if(!charge(512)||!charge(operation.idempotency_key.size())||!charge(operation.normalized_request_bytes.size())||
    !charge(operation.operation_state_bytes.empty()?0:4)||!charge(operation.operation_state_bytes.size())||
    !charge(operation.steps.size(),256))return false;
  for(const auto& step:operation.steps)if(!charge(step.idempotency_key.size()))return false;
  return true;
}
inline E CommonError(storage::database::NativeManagementOperationError error) noexcept {
  using C=storage::database::NativeManagementOperationError;
  return error==C::resource_exhausted?E::resource_exhausted:error==C::invalid_workspace?E::invalid_workspace:E::operation_failure;
}
}
inline NativePolicyActivationStateError ValidateNativePolicyActivationState(const NativePolicyActivationState& state) noexcept {
  using namespace native_policy_activation_detail;
  for(std::size_t i=0;i<identities.size();++i){const auto& id=state.*identities[i];
    if((i<7||!id.is_nil())&&!core::uuid::IsEngineIdentityUuid(id))return E::invalid_identity;
  }
  constexpr std::array<unsigned,6> distinct{0,1,2,3,4,6};
  for(std::size_t a=0;a<distinct.size();++a)for(std::size_t b=a+1;b<distinct.size();++b)
    if(state.*identities[distinct[a]]==state.*identities[distinct[b]])return E::invalid_identity;
  for(unsigned i=0;i<5;++i)if(state.boundary_uuid==state.*identities[i])return E::invalid_identity;
  if(state.boundary_uuid==state.creating_transaction_uuid&&state.boundary!=B::new_transaction)return E::invalid_identity;
  if(!state.cluster_uuid.is_nil())for(unsigned i=0;i<8;++i)
    if(state.cluster_uuid==state.*identities[i])return E::invalid_identity;
  if(!state.predecessor_uuid.is_nil())for(std::size_t i=0;i<7;++i)
    if(state.predecessor_uuid==state.*identities[i])return E::invalid_predecessor;
  if(!state.policy_generation||!state.body_generation||!state.sequence||!state.creating_local_transaction_id||
    (state.action!=A::activate&&state.action!=A::revoke)||u64(state.boundary)<1||u64(state.boundary)>6||
    state.cluster_epoch.has_value()==state.cluster_uuid.is_nil()||
    (state.boundary==B::cluster_epoch&&state.cluster_uuid.is_nil()))return E::invalid_state;
  if((state.sequence==1)!=state.predecessor_uuid.is_nil()||(state.sequence==1&&state.action!=A::activate))
    return E::invalid_predecessor;
  return E::none;
}
inline NativePolicyActivationStateImage EncodeNativePolicyActivationStateInto(
    const NativePolicyActivationState& state,core::platform::u64 budget,std::span<core::platform::byte> output) noexcept {
  using namespace native_policy_activation_detail;
  if(!storage::disk::detail::DisjointNativeDecodeRegions(output,std::span{&state,1}))return {E::invalid_workspace};
  if(budget<256||output.size()<256)return {E::resource_exhausted};
  const auto valid=ValidateNativePolicyActivationState(state);if(valid!=E::none)return {valid};
  auto bytes=output.first(256);std::fill(bytes.begin(),bytes.end(),0);std::copy_n("SBPA0001",8,bytes.begin());
  StoreLittle16(bytes.data()+8,1);StoreLittle16(bytes.data()+10,256);StoreLittle32(bytes.data()+12,256);
  for(std::size_t i=0;i<identities.size();++i){const auto& id=state.*identities[i];std::copy(id.bytes.begin(),id.bytes.end(),bytes.begin()+16+16*i);}
  StoreLittle64(bytes.data()+160,state.policy_generation);StoreLittle64(bytes.data()+168,state.body_generation);
  StoreLittle64(bytes.data()+176,state.sequence);StoreLittle64(bytes.data()+184,state.creating_local_transaction_id);
  StoreLittle64(bytes.data()+192,state.cluster_epoch.value_or(0));StoreLittle16(bytes.data()+200,u16(state.action));
  StoreLittle16(bytes.data()+202,u16(state.boundary));StoreLittle32(bytes.data()+204,state.cluster_epoch?1:0);
  return {E::none,state,bytes};
}
inline NativePolicyActivationStateImage DecodeNativePolicyActivationState(
    std::span<const core::platform::byte> bytes,core::platform::u64 budget) noexcept {
  using namespace native_policy_activation_detail;
  if(!Region(bytes))return {E::invalid_workspace};
  if(bytes.size()>budget)return {E::resource_exhausted};
  if(bytes.size()!=256||!std::equal(bytes.begin(),bytes.begin()+8,"SBPA0001")||
    LoadLittle16(bytes.data()+8)!=1||LoadLittle16(bytes.data()+10)!=256||LoadLittle32(bytes.data()+12)!=256||
    LoadLittle32(bytes.data()+204)>1||std::any_of(bytes.begin()+208,bytes.end(),[](byte b){return b!=0;}))return {E::invalid_header};
  S state;for(std::size_t i=0;i<identities.size();++i)std::copy_n(bytes.data()+16+16*i,16,(state.*identities[i]).bytes.begin());
  state.policy_generation=LoadLittle64(bytes.data()+160);state.body_generation=LoadLittle64(bytes.data()+168);
  state.sequence=LoadLittle64(bytes.data()+176);state.creating_local_transaction_id=LoadLittle64(bytes.data()+184);
  const auto epoch=LoadLittle64(bytes.data()+192);if(LoadLittle32(bytes.data()+204))state.cluster_epoch=epoch;
  else if(epoch)return {E::invalid_state};
  state.action=A(LoadLittle16(bytes.data()+200));state.boundary=B(LoadLittle16(bytes.data()+202));
  const auto valid=ValidateNativePolicyActivationState(state);if(valid!=E::none)return {valid};
  return {E::none,state,bytes};
}
// Both inputs remain declarations. The caller must resolve the predecessor
// from the boundary owner's actually selected, committed MGA activation.
inline NativePolicyActivationStateError ValidateNativePolicyActivationSuccessor(
    const NativePolicyActivationState& before,const NativePolicyActivationState& after) noexcept {
  using namespace native_policy_activation_detail;
  auto valid=ValidateNativePolicyActivationState(before);if(valid!=E::none)return valid;
  valid=ValidateNativePolicyActivationState(after);if(valid!=E::none)return valid;
  if(before.sequence==UINT64_MAX||after.sequence!=before.sequence+1||after.predecessor_uuid!=before.operation_uuid||
    after.database_uuid!=before.database_uuid||after.policy_uuid!=before.policy_uuid||after.boundary!=before.boundary||
    after.boundary_uuid!=before.boundary_uuid||after.cluster_uuid!=before.cluster_uuid||after.cluster_epoch!=before.cluster_epoch||
    ((after.creating_transaction_uuid==before.creating_transaction_uuid)!=
      (after.creating_local_transaction_id==before.creating_local_transaction_id))||
    after.policy_generation<before.policy_generation||
    (after.policy_generation==before.policy_generation&&!SameTuple(before,after))||
    (after.policy_generation>before.policy_generation&&after.policy_version_uuid==before.policy_version_uuid)||
    (after.action==A::revoke&&(before.action!=A::activate||!SameTuple(before,after))))return E::invalid_predecessor;
  return E::none;
}
inline NativePolicyActivationStateError BindNativePolicyActivationDescriptor(
    const NativePolicyActivationState& state,const core::catalog::CatalogPolicyDescriptor& descriptor) noexcept {
  using namespace native_policy_activation_detail;
  const auto valid=ValidateNativePolicyActivationState(state);if(valid!=E::none)return valid;
  if(!core::catalog::CatalogPolicyDescriptorEncodedBytes(descriptor)||state.database_uuid!=descriptor.database_uuid||
    state.policy_uuid!=descriptor.policy_uuid||state.policy_version_uuid!=descriptor.policy_version_uuid||
    state.policy_generation!=descriptor.generation||state.body_uuid!=descriptor.body_uuid||state.body_generation!=descriptor.body_generation||
    state.boundary!=descriptor.activation_boundary||state.cluster_uuid!=descriptor.cluster_uuid.value_or(core::platform::Uuid{}))return E::binding_mismatch;
  return E::none;
}
namespace native_policy_activation_detail {
inline NativePolicyActivationStateImage Bound(const M& operation,u64 budget) noexcept {
  auto state=DecodeNativePolicyActivationState(operation.operation_state_bytes,budget);if(!state.ok())return state;
  const auto& s=*state.record;using Scope=storage::database::NativeManagementScope;
  if(s.operation_uuid!=operation.uuid||s.database_uuid!=operation.database_uuid||s.policy_uuid!=operation.target_uuid||
    s.boundary_uuid!=operation.boundary_uuid||s.cluster_uuid!=operation.cluster_uuid||s.cluster_epoch!=operation.generation_guards[3]||
    (operation.scope==Scope::cluster)!=!s.cluster_uuid.is_nil()||!operation.evidence_required||
    !std::equal(operation.normalized_request_bytes.begin(),operation.normalized_request_bytes.end(),
      operation.operation_state_bytes.begin(),operation.operation_state_bytes.end()))return {E::binding_mismatch};
  return state;
}
}
inline NativePolicyActivationStateImage ReadNativePolicyActivationStateFromOperationInto(
    const storage::database::NativeManagementOperationView& operation,core::platform::u64 maximum_record_bytes,
    std::span<core::platform::Uuid> identity_scratch) noexcept {
  using namespace native_policy_activation_detail;
  if(!Disjoint(operation,identity_scratch))return {E::invalid_workspace};
  if(!Fits(operation,maximum_record_bytes))return {E::resource_exhausted};
  const auto common=storage::database::ValidateNativeManagementOperationView(operation,identity_scratch);
  if(common!=storage::database::NativeManagementOperationError::none)return {CommonError(common),{},{},common};
  return Bound(operation,maximum_record_bytes);
}
struct NativePolicyActivationEvolutionResult {
  NativePolicyActivationStateError error=NativePolicyActivationStateError::immutable_intent;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  bool ok()const noexcept{return error==NativePolicyActivationStateError::none;}
};
inline NativePolicyActivationEvolutionResult ValidateNativePolicyActivationManagementEvolution(
    const storage::database::NativeManagementOperationView& before,const storage::database::NativeManagementOperationView& after,
    core::platform::u64 maximum_record_bytes,std::span<core::platform::Uuid> identity_scratch) noexcept {
  using namespace native_policy_activation_detail;
  if(!Disjoint(before,identity_scratch)||!Disjoint(after,identity_scratch))return {E::invalid_workspace};
  if(!Fits(before,maximum_record_bytes)||!Fits(after,maximum_record_bytes))return {E::resource_exhausted};
  const auto common=storage::database::ValidateNativeManagementOperationEvolutionView(before,after,identity_scratch);
  if(common!=storage::database::NativeManagementOperationError::none)return {CommonError(common),common};
  const auto a=Bound(before,maximum_record_bytes);if(!a.ok())return {a.error,a.operation_error};
  const auto b=Bound(after,maximum_record_bytes);if(!b.ok())return {b.error,b.operation_error};
  return {*a.record==*b.record?E::none:E::immutable_intent};
}
} // namespace scratchbird::engine::internal_api
