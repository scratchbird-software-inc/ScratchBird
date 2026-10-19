// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_state.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace scratchbird::engine::internal_api {
// MGA-NATIVE-FIRST-STARTUP-TRANSACTION-DETAIL-001. Persisted historical data;
// copied identifiers cannot create a principal/session/snapshot or finality.
struct NativeStartupTransactionDetail {
  core::platform::Uuid transaction_uuid,database_uuid,engine_instance_uuid,principal_uuid;
  core::platform::Uuid attachment_uuid,session_uuid,policy_snapshot_uuid,security_snapshot_uuid;
  core::platform::Uuid visibility_epoch_uuid,isolation_profile_uuid,operation_uuid,data_snapshot_uuid;
  core::platform::Uuid evidence_root_uuid,isolation_version_uuid,transaction_policy_uuid,transaction_policy_version_uuid;
  core::platform::u64 local_transaction_id=0,started_at_micros=0,completed_at_micros=0;
  core::platform::u64 isolation_generation=0,transaction_policy_generation=0;
  core::platform::u16 authority_class=6,intent=5,read_only=0;
  transaction::mga::TransactionState final_state=transaction::mga::TransactionState::none;
  // Legacy v1 absence is unknown, never proof that no admission effect exists.
  std::optional<bool> admission_side_effect_started;
  bool operator==(const NativeStartupTransactionDetail&) const=default;
};
inline constexpr std::size_t kNativeStartupTransactionDetailBytes=320;
inline constexpr std::size_t kNativeStartupTransactionDetailV2Bytes=328;
inline constexpr std::size_t kNativeStartupTransactionDetailMaxBytes=kNativeStartupTransactionDetailV2Bytes;
inline std::size_t NativeStartupTransactionDetailEncodedBytes(const NativeStartupTransactionDetail& d) noexcept {
  return d.admission_side_effect_started.has_value()?kNativeStartupTransactionDetailV2Bytes:kNativeStartupTransactionDetailBytes;
}
enum class NativeStartupDetailError {none,invalid_header,invalid_identity,invalid_record,immutable_field,invalid_transition,invalid_workspace,resource_exhausted};
struct NativeStartupDetailImage {
  NativeStartupDetailError error=NativeStartupDetailError::invalid_header;
  std::optional<NativeStartupTransactionDetail> record;
  std::span<const core::platform::byte> bytes;
  bool ok()const noexcept{return error==NativeStartupDetailError::none&&record.has_value();}
};
namespace native_startup_detail {
using D=NativeStartupTransactionDetail;using E=NativeStartupDetailError;using Uuid=core::platform::Uuid;
using S=transaction::mga::TransactionState;
inline constexpr std::array<Uuid D::*,16> identities{
  &D::transaction_uuid,&D::database_uuid,&D::engine_instance_uuid,&D::principal_uuid,
  &D::attachment_uuid,&D::session_uuid,&D::policy_snapshot_uuid,&D::security_snapshot_uuid,
  &D::visibility_epoch_uuid,&D::isolation_profile_uuid,&D::operation_uuid,&D::data_snapshot_uuid,
  &D::evidence_root_uuid,&D::isolation_version_uuid,&D::transaction_policy_uuid,&D::transaction_policy_version_uuid};
inline constexpr std::array<core::platform::u64 D::*,5> numbers{
  &D::local_transaction_id,&D::started_at_micros,&D::completed_at_micros,&D::isolation_generation,&D::transaction_policy_generation};
inline bool Final(S s)noexcept{return s==S::committed||s==S::rolled_back||s==S::failed_terminal;}
static_assert(core::platform::u16(S::none)==0&&core::platform::u16(S::committed)==6&&
    core::platform::u16(S::rolled_back)==8&&core::platform::u16(S::failed_terminal)==11);
}
inline NativeStartupDetailError ValidateNativeStartupTransactionDetail(const NativeStartupTransactionDetail& d) noexcept {
  using namespace native_startup_detail;
  for(std::size_t i=0;i<identities.size();++i){const auto& id=d.*identities[i];
    if(i==12&&id.is_nil())continue;
    if(!core::uuid::IsEngineIdentityUuid(id))return E::invalid_identity;
    for(std::size_t j=0;j<i;++j)if(id==d.*identities[j])return E::invalid_identity;
  }
  if(d.local_transaction_id<=1||d.local_transaction_id==UINT64_MAX||!d.started_at_micros||
      !d.isolation_generation||!d.transaction_policy_generation||d.authority_class!=6||d.intent!=5||d.read_only||
      (d.final_state!=S::none&&!Final(d.final_state)))return E::invalid_record;
  if(d.final_state==S::none){if(d.completed_at_micros)return E::invalid_record;}
  else if(d.completed_at_micros<d.started_at_micros||d.evidence_root_uuid.is_nil())return E::invalid_record;
  if(d.admission_side_effect_started==false&&(d.final_state==S::committed||d.final_state==S::rolled_back))return E::invalid_record;
  return E::none;
}
inline NativeStartupDetailError ValidateNativeStartupTransactionDetailEvolution(
    const NativeStartupTransactionDetail& a,const NativeStartupTransactionDetail& b) noexcept {
  using namespace native_startup_detail;
  if(const auto e=ValidateNativeStartupTransactionDetail(a);e!=E::none)return e;
  if(const auto e=ValidateNativeStartupTransactionDetail(b);e!=E::none)return e;
  if(a==b)return E::none;
  auto expected=a;expected.evidence_root_uuid=b.evidence_root_uuid;
  expected.completed_at_micros=b.completed_at_micros;expected.final_state=b.final_state;
  expected.admission_side_effect_started=b.admission_side_effect_started;
  if(expected!=b)return E::immutable_field;
  if(Final(a.final_state)||(!a.evidence_root_uuid.is_nil()&&a.evidence_root_uuid!=b.evidence_root_uuid))return E::invalid_transition;
  if((a.admission_side_effect_started.has_value()&&!b.admission_side_effect_started.has_value())||
      (b.admission_side_effect_started==false&&a.admission_side_effect_started!=false))return E::invalid_transition;
  return E::none;
}
inline NativeStartupDetailImage EncodeNativeStartupTransactionDetailInto(
    const NativeStartupTransactionDetail& d,std::span<core::platform::byte> output) noexcept {
  using namespace native_startup_detail;using namespace core::platform;
  const auto fail=[](E e){return NativeStartupDetailImage{e,{},{}};};
  if(!storage::disk::detail::DisjointNativeDecodeRegions(output,std::span{&d,1}))return fail(E::invalid_workspace);
  const auto size=NativeStartupTransactionDetailEncodedBytes(d);
  if(output.size()<size)return fail(E::resource_exhausted);
  if(const auto e=ValidateNativeStartupTransactionDetail(d);e!=E::none)return fail(e);
  const bool marker=d.admission_side_effect_started.has_value();
  auto b=output.first(size);std::fill(b.begin(),b.end(),0);std::copy_n(marker?"SBTXD002":"SBTXD001",8,b.begin());
  StoreLittle16(b.data()+8,marker?2:1);StoreLittle16(b.data()+10,size);StoreLittle32(b.data()+12,size);
  for(std::size_t i=0;i<identities.size();++i)std::copy_n((d.*identities[i]).bytes.begin(),16,b.begin()+16+16*i);
  for(std::size_t i=0;i<numbers.size();++i)StoreLittle64(b.data()+272+8*i,d.*numbers[i]);
  StoreLittle16(b.data()+312,d.authority_class);StoreLittle16(b.data()+314,d.intent);
  StoreLittle16(b.data()+316,d.read_only);StoreLittle16(b.data()+318,u16(d.final_state));
  if(marker)b[320]=*d.admission_side_effect_started?1:0;
  return {E::none,d,b};
}
inline NativeStartupDetailImage DecodeNativeStartupTransactionDetail(std::span<const core::platform::byte> b) noexcept {
  using namespace native_startup_detail;using namespace core::platform;
  const auto fail=[](E e){return NativeStartupDetailImage{e,{},{}};};
  if(!storage::disk::detail::DecodedRange(b).valid)return fail(E::invalid_workspace);
  if(b.size()!=kNativeStartupTransactionDetailBytes&&b.size()!=kNativeStartupTransactionDetailV2Bytes)return fail(E::invalid_header);
  const bool marker=b.size()==kNativeStartupTransactionDetailV2Bytes;
  if(!std::equal(b.begin(),b.begin()+8,marker?"SBTXD002":"SBTXD001")||
      LoadLittle16(b.data()+8)!=(marker?2:1)||LoadLittle16(b.data()+10)!=b.size()||LoadLittle32(b.data()+12)!=b.size()||
      (marker&&(b[320]>1||!std::all_of(b.begin()+321,b.end(),[](byte value){return value==0;}))))return fail(E::invalid_header);
  D d;for(std::size_t i=0;i<identities.size();++i)std::copy_n(b.data()+16+16*i,16,(d.*identities[i]).bytes.begin());
  for(std::size_t i=0;i<numbers.size();++i)d.*numbers[i]=LoadLittle64(b.data()+272+8*i);
  d.authority_class=LoadLittle16(b.data()+312);d.intent=LoadLittle16(b.data()+314);
  d.read_only=LoadLittle16(b.data()+316);d.final_state=S(LoadLittle16(b.data()+318));
  if(marker)d.admission_side_effect_started=b[320]!=0;
  if(const auto e=ValidateNativeStartupTransactionDetail(d);e!=E::none)return fail(e);
  return {E::none,d,b};
}
} // namespace scratchbird::engine::internal_api
