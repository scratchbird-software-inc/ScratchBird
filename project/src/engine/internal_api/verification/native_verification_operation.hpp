// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../transaction/native_startup_transaction_detail.hpp"
#include "../transaction/native_startup_admission_evidence.hpp"
#include "native_management_operation.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "uuid.hpp"
#include "native_verification_classes.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace scratchbird::engine::internal_api {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u16;
using core::platform::u32;
using core::platform::u64;

enum class NativeVerificationPhase : u16 {
  admitted=1,boundary_established,identity_verified,allocation_verifying,
  transaction_inventory_verifying,catalog_verifying,record_chain_verifying,
  index_verifying,constraint_verifying,archive_backup_verifying,cluster_verifying,
  classifying_findings,repair_planning,repair_running,post_repair_verify,
  evidence_finalizing,quarantine_running,salvage_assessing,salvage_running,
  review_required,completed,failed
};
enum class NativeVerificationCompletion : u16 {
  running=1,paused,cancel_requested,completed_clean,completed_findings,
  completed_repaired,failed,quarantined,review_required
};
struct NativeVerificationPageRange {
  Uuid filespace_uuid;
  u64 first_page=0,page_count=0;
  bool operator==(const NativeVerificationPageRange&) const=default;
};
// Typed durable data, not an authority constructor or completion receipt. The
// engine must resolve the real lineage, policies, targets and boundary before
// accepting this declaration. The creation lineage may equal the database UUID
// when the actual source declares that default; absence is not such a proof.
// No persisted "completed" enum proves execution.
struct NativeVerificationOperationState {
  Uuid operation_uuid,database_uuid,database_lineage_uuid;
  Uuid repair_policy_uuid,quarantine_policy_uuid,salvage_policy_uuid,evidence_policy_uuid;
  Uuid acting_subject_uuid,snapshot_boundary_uuid,maintenance_boundary_uuid,cluster_uuid;
  std::optional<u64> cluster_epoch;
  u64 created_at_micros=0,updated_at_micros=0;
  NativeVerificationClass verification_class=NativeVerificationClass::startup_full;
  NativeVerificationAuthority authority_class=NativeVerificationAuthority::startup_recovery;
  NativeVerificationPhase phase=NativeVerificationPhase::admitted;
  NativeVerificationCompletion completion=NativeVerificationCompletion::running;
  // Core SB-MGA-VERIFY-LEVELS order, bit 0 metadata_only through bit 9
  // salvage_assessment. A nonempty explicit set is mandatory.
  u32 verification_levels=0;
  std::span<const Uuid> filespaces,objects;
  std::span<const NativeVerificationPageRange> page_ranges;
  std::string_view last_verified_boundary;
  std::optional<NativeStartupTransactionDetail> transaction_detail;
  std::span<const byte> startup_admission_evidence;
};
enum class NativeVerificationStateError {
  none,invalid_header,invalid_identity,invalid_state,invalid_targets,
  invalid_boundary,invalid_transition,immutable_field,resource_exhausted,invalid_workspace,
  operation_failure,binding_mismatch
};
struct NativeVerificationStateWorkspace {
  std::span<Uuid> filespaces,objects;
  std::span<NativeVerificationPageRange> page_ranges;
};
struct NativeVerificationStateImage {
  NativeVerificationStateError error=NativeVerificationStateError::invalid_header;
  std::optional<NativeVerificationOperationState> record;
  std::span<const byte> bytes;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  bool ok() const noexcept{return error==NativeVerificationStateError::none&&record.has_value();}
};
namespace native_verification_state_detail {
using E=NativeVerificationStateError;
using P=NativeVerificationPhase;
using C=NativeVerificationCompletion;
using O=NativeVerificationOperationState;
inline constexpr std::array<Uuid O::*,11> identities{
  &O::operation_uuid,&O::database_uuid,&O::database_lineage_uuid,&O::repair_policy_uuid,
  &O::quarantine_policy_uuid,&O::salvage_policy_uuid,&O::evidence_policy_uuid,
  &O::acting_subject_uuid,&O::snapshot_boundary_uuid,&O::maintenance_boundary_uuid,&O::cluster_uuid};
inline bool Terminal(P phase) noexcept {return phase==P::completed||phase==P::failed||phase==P::review_required;}
inline bool Utf8(std::string_view text) noexcept {
  if(text.size()>4096||!storage::disk::detail::DecodedRange(std::span{text.data(),text.size()}).valid)return false;
  for(std::size_t at=0;at<text.size();){const auto first=static_cast<byte>(text[at++]);u32 value=first;unsigned extra=0;
    if(first>=0xc2&&first<=0xdf){value=first&31;extra=1;}
    else if(first>=0xe0&&first<=0xef){value=first&15;extra=2;}
    else if(first>=0xf0&&first<=0xf4){value=first&7;extra=3;}
    else if(first>=0x80)return false;
    if(extra>text.size()-at)return false;
    for(unsigned i=0;i<extra;++i){const auto next=static_cast<byte>(text[at++]);if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&63);}
    if((extra==1&&value<0x80)||(extra==2&&value<0x800)||(extra==3&&value<0x10000)||
        value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
  }
  return true;
}
inline bool Ordered(std::span<const Uuid> ids) noexcept {
  for(std::size_t i=0;i<ids.size();++i)
    if(!core::uuid::IsEngineIdentityUuid(ids[i])||(i&&!(ids[i-1].bytes<ids[i].bytes)))return false;
  return true;
}
inline bool Size(const O& o,u64 budget,std::size_t& size) noexcept {
  const u64 ceiling=std::min<u64>(budget,std::numeric_limits<u32>::max());
  u64 used=256+(o.transaction_detail?NativeStartupTransactionDetailEncodedBytes(*o.transaction_detail):0);
  const auto add=[&](std::size_t count,u64 width){if(used>ceiling||count>(ceiling-used)/width)return false;used+=count*width;return true;};
  if(!add(o.startup_admission_evidence.size(),1)||!add(o.filespaces.size(),16)||!add(o.objects.size(),16)||!add(o.page_ranges.size(),32)||!add(o.last_verified_boundary.size(),1))return false;
  size=static_cast<std::size_t>(used);return true;
}
inline bool Edge(P from,P to) noexcept {
  if(Terminal(from))return false;
  if(from==to||to==P::failed||to==P::review_required||to==P::quarantine_running||to==P::salvage_assessing)return true;
  switch(from){
    case P::admitted:return to==P::boundary_established;
    case P::boundary_established:return to==P::identity_verified;
    case P::identity_verified:return to==P::allocation_verifying;
    case P::allocation_verifying:return to==P::transaction_inventory_verifying;
    case P::transaction_inventory_verifying:return to==P::catalog_verifying;
    case P::catalog_verifying:return to==P::record_chain_verifying;
    case P::record_chain_verifying:return to==P::index_verifying;
    case P::index_verifying:return to==P::constraint_verifying;
    case P::constraint_verifying:return to==P::archive_backup_verifying;
    case P::archive_backup_verifying:return to==P::cluster_verifying||to==P::classifying_findings;
    case P::cluster_verifying:return to==P::classifying_findings;
    case P::classifying_findings:return to==P::repair_planning;
    case P::repair_planning:return to==P::repair_running||to==P::evidence_finalizing;
    case P::repair_running:return to==P::post_repair_verify;
    case P::post_repair_verify:return to==P::evidence_finalizing;
    case P::evidence_finalizing:return to==P::completed;
    case P::quarantine_running:return to==P::evidence_finalizing;
    case P::salvage_assessing:return to==P::salvage_running;
    case P::salvage_running:return to==P::evidence_finalizing;
    default:return false;
  }
}
template<class T> bool Same(std::span<const T> a,std::span<const T> b) noexcept {
  return std::equal(a.begin(),a.end(),b.begin(),b.end());
}
inline bool Equal(const O& a,const O& b) noexcept {
  for(const auto member:identities)if(a.*member!=b.*member)return false;
  return a.cluster_epoch==b.cluster_epoch&&a.created_at_micros==b.created_at_micros&&a.updated_at_micros==b.updated_at_micros&&
    a.verification_class==b.verification_class&&a.authority_class==b.authority_class&&a.phase==b.phase&&
    a.completion==b.completion&&a.verification_levels==b.verification_levels&&Same(a.filespaces,b.filespaces)&&
    Same(a.objects,b.objects)&&Same(a.page_ranges,b.page_ranges)&&a.last_verified_boundary==b.last_verified_boundary&&
    a.transaction_detail==b.transaction_detail&&Same(a.startup_admission_evidence,b.startup_admission_evidence);
}
// Framing only; the full decoder still verifies every byte, length and field.
inline std::size_t PrefixBytes(std::span<const byte> b) noexcept {
  using namespace core::platform;
  if(!storage::disk::detail::DecodedRange(b).valid||b.size()<256||LoadLittle16(b.data()+10)!=256)return 0;
  if(std::equal(b.begin(),b.begin()+8,"SBVR0001")&&LoadLittle16(b.data()+8)==1&&
      std::all_of(b.begin()+248,b.begin()+256,[](byte v){return v==0;}))return 256;
  const auto detail=LoadLittle32(b.data()+248);
  if(std::equal(b.begin(),b.begin()+8,"SBVR0002")&&LoadLittle16(b.data()+8)==2&&
      (detail==kNativeStartupTransactionDetailBytes||detail==kNativeStartupTransactionDetailV2Bytes)&&
      LoadLittle32(b.data()+252)==0&&b.size()>=256+detail)return 256+detail;
  const auto evidence=LoadLittle32(b.data()+252);
  if(std::equal(b.begin(),b.begin()+8,"SBVR0003")&&LoadLittle16(b.data()+8)==3&&
      detail==kNativeStartupTransactionDetailV2Bytes&&evidence>=kNativeAdmissionEvidenceHeaderBytes&&
      evidence<=kNativeAdmissionEvidenceMaximumBytes&&u64(256)+detail+evidence<=b.size())return 256+detail+evidence;
  return 0;
}
}
inline NativeVerificationStateError ValidateNativeVerificationOperationState(const NativeVerificationOperationState& o) noexcept {
  using namespace native_verification_state_detail;
  if(!Region(o.filespaces)||!Region(o.objects)||!Region(o.page_ranges)||!Region(o.startup_admission_evidence))return E::invalid_workspace;
  for(std::size_t n=0;n<identities.size();++n){const auto& id=o.*identities[n];
    if((n<7||!id.is_nil())&&!core::uuid::IsEngineIdentityUuid(id))return E::invalid_identity;
  }
  if(o.operation_uuid==o.database_uuid||o.operation_uuid==o.database_lineage_uuid)return E::invalid_identity;
  if(u16(o.verification_class)<1||u16(o.verification_class)>9||u16(o.authority_class)<1||u16(o.authority_class)>6||
      u16(o.phase)<1||u16(o.phase)>22||u16(o.completion)<1||u16(o.completion)>9||
      !o.verification_levels||(o.verification_levels&~1023u)||!o.created_at_micros||o.updated_at_micros<o.created_at_micros)return E::invalid_state;
  if(o.cluster_epoch.has_value()==o.cluster_uuid.is_nil()||
      (o.verification_class==NativeVerificationClass::cluster&&o.cluster_uuid.is_nil())||
      (o.phase==P::cluster_verifying&&o.cluster_uuid.is_nil()))return E::invalid_state;
  if(o.phase==P::completed){if(o.completion!=C::completed_clean&&o.completion!=C::completed_findings&&
      o.completion!=C::completed_repaired&&o.completion!=C::quarantined)return E::invalid_state;}
  else if(o.phase==P::failed){if(o.completion!=C::failed)return E::invalid_state;}
  else if(o.phase==P::review_required){if(o.completion!=C::review_required)return E::invalid_state;}
  else if(o.completion!=C::running&&o.completion!=C::paused&&o.completion!=C::cancel_requested)return E::invalid_state;
  if(o.filespaces.empty()||!Ordered(o.filespaces)||!Ordered(o.objects))return E::invalid_targets;
  for(std::size_t n=0;n<o.page_ranges.size();++n){const auto& range=o.page_ranges[n];
    if(!range.page_count||range.page_count-1>std::numeric_limits<u64>::max()-range.first_page||
        !std::binary_search(o.filespaces.begin(),o.filespaces.end(),range.filespace_uuid,[](const Uuid& a,const Uuid& b){return a.bytes<b.bytes;}))return E::invalid_targets;
    if(n){const auto& previous=o.page_ranges[n-1];
      if(range.filespace_uuid.bytes<previous.filespace_uuid.bytes||
          (range.filespace_uuid==previous.filespace_uuid&&range.first_page<=previous.first_page+previous.page_count-1))return E::invalid_targets;}
  }
  if(!Utf8(o.last_verified_boundary))return E::invalid_boundary;
  if(o.phase!=P::admitted&&o.phase!=P::failed&&o.phase!=P::review_required){
    if(o.snapshot_boundary_uuid.is_nil()&&o.maintenance_boundary_uuid.is_nil()&&o.cluster_uuid.is_nil())return E::invalid_boundary;
    if(o.verification_class==NativeVerificationClass::online_consistency&&o.snapshot_boundary_uuid.is_nil())return E::invalid_boundary;
    if((o.verification_class==NativeVerificationClass::startup_light||o.verification_class==NativeVerificationClass::startup_full||
        o.verification_class==NativeVerificationClass::offline_consistency)&&o.maintenance_boundary_uuid.is_nil())return E::invalid_boundary;
  }
  if(o.transaction_detail){const auto& d=*o.transaction_detail;
    if(ValidateNativeStartupTransactionDetail(d)!=NativeStartupDetailError::none||
        o.authority_class!=NativeVerificationAuthority::startup_recovery||
        (o.verification_class!=NativeVerificationClass::startup_light&&o.verification_class!=NativeVerificationClass::startup_full)||
        o.phase==P::admitted||o.maintenance_boundary_uuid.is_nil()||!o.cluster_uuid.is_nil()||o.cluster_epoch||
        d.database_uuid!=o.database_uuid||d.operation_uuid!=o.operation_uuid||d.principal_uuid!=o.acting_subject_uuid||
        d.data_snapshot_uuid!=o.snapshot_boundary_uuid||d.started_at_micros!=o.created_at_micros||
        d.completed_at_micros>o.updated_at_micros)return E::binding_mismatch;
  }
  if(!o.startup_admission_evidence.empty()){
    if(!o.transaction_detail||o.transaction_detail->admission_side_effect_started!=true)return E::binding_mismatch;
    const auto evidence=DecodeNativeStartupAdmissionEvidence(o.startup_admission_evidence,kNativeAdmissionEvidenceMaximumBytes);
    if(!evidence.ok())return E::invalid_state;
    const auto& a=*evidence.record;const auto& d=*o.transaction_detail;
    using I=NativeAdmissionIdentity;using N=NativeAdmissionNumber;
    if(a.Identity(I::database)!=d.database_uuid||a.Identity(I::operation)!=d.operation_uuid||
        a.Identity(I::transaction)!=d.transaction_uuid||a.Identity(I::actor)!=d.principal_uuid||
        a.Identity(I::engine)!=d.engine_instance_uuid||a.Identity(I::session)!=d.session_uuid||
        a.Identity(I::policy_snapshot)!=d.policy_snapshot_uuid||a.Identity(I::security_snapshot)!=d.security_snapshot_uuid||
        a.Identity(I::data_snapshot)!=d.data_snapshot_uuid||a.Identity(I::evidence_policy)!=o.evidence_policy_uuid||
        a.Number(N::transaction_number)!=d.local_transaction_id||
        a.Number(N::authorization_time_utc_us)<o.created_at_micros||a.Number(N::policy_time_utc_us)>o.updated_at_micros||
        !std::binary_search(o.filespaces.begin(),o.filespaces.end(),a.Identity(I::storage_filespace)))return E::binding_mismatch;
    bool transaction_policy=false;
    for(std::size_t n=0;n<a.count(3);++n){const auto policy=a.Policy(n);
      if(policy.policy_uuid==d.transaction_policy_uuid&&policy.body_generation==d.transaction_policy_generation)transaction_policy=true;
    }
    bool body_version=false;for(std::size_t n=0;n<a.count(2);++n)if(a.Item(2,n)==d.transaction_policy_version_uuid)body_version=true;
    if(!transaction_policy||!body_version)return E::binding_mismatch;
    using S=transaction::mga::TransactionState;
    if(a.outcome()!=NativeStartupPolicyOutcome::allow&&
        (d.final_state==S::committed||(o.phase!=P::boundary_established&&o.phase!=P::failed&&o.phase!=P::review_required)))return E::binding_mismatch;
  }
  return E::none;
}
inline NativeVerificationStateError ValidateNativeVerificationOperationEvolution(
    const NativeVerificationOperationState& a,const NativeVerificationOperationState& b) noexcept {
  using namespace native_verification_state_detail;
  const auto first=ValidateNativeVerificationOperationState(a);if(first!=E::none)return first;
  const auto second=ValidateNativeVerificationOperationState(b);if(second!=E::none)return second;
  if(Equal(a,b))return E::none;
  if(!Edge(a.phase,b.phase)||b.updated_at_micros<a.updated_at_micros)return E::invalid_transition;
  if(a.transaction_detail){
    if(!b.transaction_detail)return E::immutable_field;
    if(ValidateNativeStartupTransactionDetailEvolution(*a.transaction_detail,*b.transaction_detail)!=NativeStartupDetailError::none)
      return E::immutable_field;
  }else if(b.transaction_detail&&(a.phase!=P::boundary_established||b.phase!=P::boundary_established||
      !a.snapshot_boundary_uuid.is_nil()||b.snapshot_boundary_uuid.is_nil()))return E::invalid_transition;
  if(!a.startup_admission_evidence.empty()){
    if(!Same(a.startup_admission_evidence,b.startup_admission_evidence))return E::immutable_field;
  }else if(!b.startup_admission_evidence.empty()&&
      (a.phase!=P::boundary_established||b.phase!=P::boundary_established||!a.transaction_detail||
       !b.transaction_detail||a.transaction_detail->admission_side_effect_started!=true||
       a.transaction_detail->final_state!=transaction::mga::TransactionState::none||
       b.transaction_detail->final_state!=transaction::mga::TransactionState::none))return E::invalid_transition;
  for(const auto member:identities){
    if(member==&O::snapshot_boundary_uuid||member==&O::maintenance_boundary_uuid){
      if(a.*member==b.*member)continue;
      const bool detail_snapshot=member==&O::snapshot_boundary_uuid&&!a.transaction_detail&&b.transaction_detail&&
          a.phase==P::boundary_established&&b.phase==P::boundary_established;
      if(!(a.*member).is_nil()||(!detail_snapshot&&(a.phase!=P::admitted||b.phase!=P::boundary_established)))return E::immutable_field;
    }else if(a.*member!=b.*member)return E::immutable_field;
  }
  if(a.cluster_epoch!=b.cluster_epoch||a.created_at_micros!=b.created_at_micros||a.verification_class!=b.verification_class||
      a.authority_class!=b.authority_class||a.verification_levels!=b.verification_levels||
      !Same(a.filespaces,b.filespaces)||!Same(a.objects,b.objects)||!Same(a.page_ranges,b.page_ranges))return E::immutable_field;
  return E::none;
}
// SBVR0001/2/3 is nested in SBMGO003's exact state bytes. The aggregate seal covers
// it; this decoder does not replace aggregate admission or engine authority.
inline NativeVerificationStateImage EncodeNativeVerificationOperationStateInto(
    const NativeVerificationOperationState& o,u64 budget,std::span<byte> output) noexcept {
  using namespace native_verification_state_detail;
  const auto fail=[](E e){return NativeVerificationStateImage{e,{},{}};};
  const auto safe=[&](auto input){return storage::disk::detail::DisjointNativeDecodeRegions(output,input);};
  if(!safe(std::span{&o,1})||!safe(o.startup_admission_evidence)||!safe(o.filespaces)||!safe(o.objects)||!safe(o.page_ranges)||
      !safe(std::span{o.last_verified_boundary.data(),o.last_verified_boundary.size()}))return fail(E::invalid_workspace);
  std::size_t size=0;if(!Size(o,budget,size)||size>output.size())return fail(E::resource_exhausted);
  const auto valid=ValidateNativeVerificationOperationState(o);if(valid!=E::none)return fail(valid);
  std::array<byte,kNativeStartupTransactionDetailMaxBytes> detail_backing{};
  std::span<const byte> detail_bytes;
  if(o.transaction_detail){const auto detail=EncodeNativeStartupTransactionDetailInto(*o.transaction_detail,detail_backing);
    if(!detail.ok())return fail(E::invalid_state);detail_bytes=detail.bytes;}
  const bool admission=!o.startup_admission_evidence.empty();
  auto b=output.first(size);std::fill(b.begin(),b.end(),0);std::copy_n(admission?"SBVR0003":o.transaction_detail?"SBVR0002":"SBVR0001",8,b.begin());
  using namespace core::platform;
  StoreLittle16(b.data()+8,admission?3:o.transaction_detail?2:1);StoreLittle16(b.data()+10,256);StoreLittle32(b.data()+12,size);
  for(std::size_t n=0;n<identities.size();++n)std::copy((o.*identities[n]).bytes.begin(),(o.*identities[n]).bytes.end(),b.begin()+16+16*n);
  StoreLittle64(b.data()+192,o.cluster_epoch.value_or(0));StoreLittle64(b.data()+200,o.created_at_micros);StoreLittle64(b.data()+208,o.updated_at_micros);
  StoreLittle16(b.data()+216,u16(o.verification_class));StoreLittle16(b.data()+218,u16(o.authority_class));
  StoreLittle16(b.data()+220,u16(o.phase));StoreLittle16(b.data()+222,u16(o.completion));
  StoreLittle32(b.data()+224,o.verification_levels);StoreLittle32(b.data()+228,o.cluster_epoch.has_value()?1:0);
  StoreLittle32(b.data()+232,o.filespaces.size());StoreLittle32(b.data()+236,o.objects.size());
  StoreLittle32(b.data()+240,o.page_ranges.size());StoreLittle32(b.data()+244,o.last_verified_boundary.size());
  std::size_t at=256;
  if(o.transaction_detail){StoreLittle32(b.data()+248,detail_bytes.size());std::copy(detail_bytes.begin(),detail_bytes.end(),b.begin()+at);at+=detail_bytes.size();}
  if(admission){StoreLittle32(b.data()+252,o.startup_admission_evidence.size());
    std::copy(o.startup_admission_evidence.begin(),o.startup_admission_evidence.end(),b.begin()+at);at+=o.startup_admission_evidence.size();}
  for(auto ids:{o.filespaces,o.objects})for(const auto& id:ids){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);at+=16;}
  for(const auto& range:o.page_ranges){std::copy(range.filespace_uuid.bytes.begin(),range.filespace_uuid.bytes.end(),b.begin()+at);
    StoreLittle64(b.data()+at+16,range.first_page);StoreLittle64(b.data()+at+24,range.page_count);at+=32;}
  std::copy(o.last_verified_boundary.begin(),o.last_verified_boundary.end(),b.begin()+at);
  return {E::none,o,b};
}
inline NativeVerificationStateImage DecodeNativeVerificationOperationStateInto(
    std::span<const byte> b,u64 budget,NativeVerificationStateWorkspace w) noexcept {
  using namespace native_verification_state_detail;using namespace core::platform;
  const auto fail=[](E e){return NativeVerificationStateImage{e,{},{}};};
  if(!storage::disk::detail::DisjointNativeDecodeRegions(b,w.filespaces,w.objects,w.page_ranges)||
      !Region(w.filespaces)||!Region(w.objects)||!Region(w.page_ranges))return fail(E::invalid_workspace);
  if(b.size()>budget||b.size()>std::numeric_limits<u32>::max())return fail(E::resource_exhausted);
  const auto prefix=PrefixBytes(b);
  if(!prefix||LoadLittle32(b.data()+12)!=b.size())return fail(E::invalid_header);
  const auto flags=LoadLittle32(b.data()+228);if(flags&~1u)return fail(E::invalid_header);
  O o;
  if(prefix>256){const auto length=LoadLittle32(b.data()+248);
    const auto detail=DecodeNativeStartupTransactionDetail(b.subspan(256,length));
    if(!detail.ok())return fail(E::invalid_state);o.transaction_detail=detail.record;
    const auto evidence_length=LoadLittle32(b.data()+252);
    if(evidence_length)o.startup_admission_evidence=b.subspan(256+length,evidence_length);
  }
  for(std::size_t n=0;n<identities.size();++n)std::copy_n(b.data()+16+16*n,16,(o.*identities[n]).bytes.begin());
  const auto epoch=LoadLittle64(b.data()+192);if(flags&1)o.cluster_epoch=epoch;else if(epoch)return fail(E::invalid_header);
  o.created_at_micros=LoadLittle64(b.data()+200);o.updated_at_micros=LoadLittle64(b.data()+208);
  o.verification_class=NativeVerificationClass(LoadLittle16(b.data()+216));o.authority_class=NativeVerificationAuthority(LoadLittle16(b.data()+218));
  o.phase=P(LoadLittle16(b.data()+220));o.completion=C(LoadLittle16(b.data()+222));o.verification_levels=LoadLittle32(b.data()+224);
  const auto files=LoadLittle32(b.data()+232),objects=LoadLittle32(b.data()+236),ranges=LoadLittle32(b.data()+240),text=LoadLittle32(b.data()+244);
  const u64 expected=prefix+16*u64(files)+16*u64(objects)+32*u64(ranges)+text;
  if(expected!=b.size())return fail(E::invalid_header);
  if(files>w.filespaces.size()||objects>w.objects.size()||ranges>w.page_ranges.size())return fail(E::resource_exhausted);
  std::size_t at=prefix;
  for(auto ids:{w.filespaces.first(files),w.objects.first(objects)})for(auto& id:ids){std::copy_n(b.data()+at,16,id.bytes.begin());at+=16;}
  for(auto& range:w.page_ranges.first(ranges)){
    std::copy_n(b.data()+at,16,range.filespace_uuid.bytes.begin());range.first_page=LoadLittle64(b.data()+at+16);
    range.page_count=LoadLittle64(b.data()+at+24);at+=32;
  }
  o.filespaces=w.filespaces.first(files);o.objects=w.objects.first(objects);o.page_ranges=w.page_ranges.first(ranges);
  o.last_verified_boundary={reinterpret_cast<const char*>(b.data()+at),text};
  const auto valid=ValidateNativeVerificationOperationState(o);if(valid!=E::none)return fail(valid);
  return {E::none,o,b};
}
namespace native_verification_state_detail {
using ManagementView=storage::database::NativeManagementOperationView;
inline bool OperationRegionsDisjoint(const ManagementView& operation,
    NativeVerificationStateWorkspace before,NativeVerificationStateWorkspace after,std::span<Uuid> identities) noexcept {
  const auto safe=[&](auto input){return storage::disk::detail::DisjointNativeDecodeRegions(input,
    before.filespaces,before.objects,before.page_ranges,after.filespaces,after.objects,after.page_ranges,identities);};
  if(!Region(operation.steps)||!Region(before.filespaces)||!Region(before.objects)||!Region(before.page_ranges)||
      !Region(after.filespaces)||!Region(after.objects)||!Region(after.page_ranges)||!Region(identities)||
      !safe(std::span{&operation,1})||!safe(operation.steps)||!safe(operation.normalized_request_bytes)||
      !safe(operation.operation_state_bytes)||!safe(std::span{operation.idempotency_key.data(),operation.idempotency_key.size()}))return false;
  for(const auto& step:operation.steps)
    if(!safe(std::span{step.idempotency_key.data(),step.idempotency_key.size()}))return false;
  return true;
}
inline bool OperationFits(const ManagementView& operation,u64 budget) noexcept {
  // Bound the complete enclosing aggregate before hashing its immutable
  // request or touching identity scratch, not merely the nested state bytes.
  const u64 ceiling=std::min<u64>(budget,std::numeric_limits<u32>::max());
  u64 used=0;
  const auto charge=[&](std::size_t count,u64 width=1){
    if(used>ceiling||count>(ceiling-used)/width)return false;used+=count*width;return true;};
  if(!charge(512)||!charge(operation.idempotency_key.size())||!charge(operation.normalized_request_bytes.size())||
      !charge(operation.operation_state_bytes.empty()?0:4)||!charge(operation.operation_state_bytes.size())||
      !charge(operation.steps.size(),256))return false;
  for(const auto& step:operation.steps)if(!charge(step.idempotency_key.size()))return false;
  return true;
}
inline NativeVerificationStateImage DecodeBoundState(const ManagementView& operation,u64 budget,
    NativeVerificationStateWorkspace workspace) noexcept {
  using S=storage::database::NativeManagementState;
  const auto fail=[](E e){return NativeVerificationStateImage{e,{},{}};};
  auto decoded=DecodeNativeVerificationOperationStateInto(operation.operation_state_bytes,budget,workspace);
  if(!decoded.ok())return decoded;
  const auto& state=*decoded.record;
  if(state.transaction_detail){const auto& d=*state.transaction_detail;
    if(d.session_uuid!=operation.request_context_uuid||d.policy_snapshot_uuid!=operation.policy_snapshot_uuid||
        d.security_snapshot_uuid!=operation.security_snapshot_uuid||d.principal_uuid!=operation.initiator_uuid)
      return fail(E::binding_mismatch);
  }
  if(state.operation_uuid!=operation.uuid||state.database_uuid!=operation.database_uuid||
      state.cluster_uuid!=operation.cluster_uuid||state.cluster_epoch!=operation.generation_guards[3]||
      (state.phase!=P::admitted&&state.phase!=P::failed&&state.phase!=P::review_required&&operation.state==S::created)||
      (state.phase==P::completed)!=(operation.state==S::completed)||
      ((operation.state==S::failed_terminal||operation.state==S::cancelled)&&state.phase!=P::failed)||
      (state.phase==P::failed&&operation.state!=S::failed_retryable&&operation.state!=S::failed_terminal&&operation.state!=S::cancelled))
    return fail(E::binding_mismatch);
  return decoded;
}
inline E CommonError(storage::database::NativeManagementOperationError error) noexcept {
  using M=storage::database::NativeManagementOperationError;
  return error==M::resource_exhausted?E::resource_exhausted:error==M::invalid_workspace?E::invalid_workspace:E::operation_failure;
}
} // namespace native_verification_state_detail
// Admit the complete common record before decoding its exact retained state.
// Policy/lineage/boundary resolution and execution authority remain separate.
inline NativeVerificationStateImage ReadNativeVerificationStateFromOperationInto(
    const storage::database::NativeManagementOperationView& operation,u64 budget,
    NativeVerificationStateWorkspace workspace,std::span<Uuid> identities) noexcept {
  using namespace native_verification_state_detail;
  const auto fail=[](E e){return NativeVerificationStateImage{e,{},{}};};
  if(!OperationRegionsDisjoint(operation,workspace,{},identities))return fail(E::invalid_workspace);
  if(!OperationFits(operation,budget))return fail(E::resource_exhausted);
  const auto valid=storage::database::ValidateNativeManagementOperationView(operation,identities);
  if(valid!=storage::database::NativeManagementOperationError::none){auto result=fail(CommonError(valid));result.operation_error=valid;return result;}
  return DecodeBoundState(operation,budget,workspace);
}
struct NativeVerificationEvolutionResult {
  NativeVerificationStateError error=NativeVerificationStateError::invalid_transition;
  storage::database::NativeManagementOperationError operation_error=storage::database::NativeManagementOperationError::none;
  bool ok() const noexcept {return error==NativeVerificationStateError::none;}
};
// One family boundary composes common revision/equality rules with the full
// typed phase graph. Both record budgets are ceilings, not memory grants. All
// writable scratch excludes BOTH immutable endpoints before any validation.
inline NativeVerificationEvolutionResult ValidateNativeVerificationManagementEvolution(
    const storage::database::NativeManagementOperationView& before,
    const storage::database::NativeManagementOperationView& after,u64 maximum_record_bytes,
    NativeVerificationStateWorkspace before_workspace,NativeVerificationStateWorkspace after_workspace,
    std::span<Uuid> identities) noexcept {
  using namespace native_verification_state_detail;
  if(!OperationRegionsDisjoint(before,before_workspace,after_workspace,identities)||
      !OperationRegionsDisjoint(after,before_workspace,after_workspace,identities))return {E::invalid_workspace};
  if(!OperationFits(before,maximum_record_bytes)||!OperationFits(after,maximum_record_bytes))return {E::resource_exhausted};
  const auto common=storage::database::ValidateNativeManagementOperationEvolutionView(before,after,identities);
  if(common!=storage::database::NativeManagementOperationError::none)return {CommonError(common),common};
  const auto a=DecodeBoundState(before,maximum_record_bytes,before_workspace);if(!a.ok())return {a.error,a.operation_error};
  const auto b=DecodeBoundState(after,maximum_record_bytes,after_workspace);if(!b.ok())return {b.error,b.operation_error};
  return {ValidateNativeVerificationOperationEvolution(*a.record,*b.record)};
}
} // namespace scratchbird::engine::internal_api
