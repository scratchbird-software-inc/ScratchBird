// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_startup_policy_types.hpp"
#include "hash_digest_parts.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::engine::internal_api {
// Closed native pre-active admission evidence profile. These bytes retain two
// registered audit events and their evidence content; decoding is not admission.
enum class NativeAdmissionIdentity : std::size_t {
  authorization_event,policy_event,evidence,database,engine,operation,transaction,
  session,actor,policy_snapshot,security_snapshot,data_snapshot,evaluation,decision,
  authorization_decision,access_policy,access_descriptor_version,access_body,access_body_version,
  evidence_policy,evidence_descriptor_version,evidence_body,evidence_body_version,
  storage_filespace,decisive_policy,count
};
enum class NativeAdmissionNumber : std::size_t {
  transaction_number,authorization_time_utc_us,policy_time_utc_us,catalog_generation,
  security_epoch,effective_policy_generation,access_generation,access_body_generation,
  evidence_generation,evidence_body_generation,selection_generation,checkpoint_generation,count
};
enum class NativeAdmissionReason : core::platform::u16 {
  startup_policy_allow=1,applicable_policy_refusal,unresolved_applicable_body,policy_generation_changed
};
enum class NativeAdmissionEvidenceError {
  none,invalid_header,invalid_identity,invalid_state,invalid_workspace,resource_exhausted,hash_failure,invalid_integrity
};
inline constexpr std::size_t kNativeAdmissionEvidenceHeaderBytes=640;
inline constexpr std::size_t kNativeAdmissionPolicyEvidenceBytes=80;
inline constexpr std::size_t kNativeAdmissionEvidenceMaximumItems=128;
inline constexpr std::size_t kNativeAdmissionEvidenceMaximumBytes=640+128*(16*3+80);
struct NativeStartupAdmissionEvidenceInput {
  std::array<core::platform::Uuid,25> identities{};
  std::array<core::platform::u64,12> numbers{};
  std::array<core::platform::byte,32> source_checkpoint_sha256{};
  NativeStartupPolicyOutcome outcome=NativeStartupPolicyOutcome::deny;
  NativeAdmissionReason reason=NativeAdmissionReason::applicable_policy_refusal;
  std::span<const core::platform::Uuid> subjects,targets,metadata_versions;
  std::span<const NativeStartupPolicyBinding> policies;
  auto& Identity(NativeAdmissionIdentity field) noexcept{return identities[std::size_t(field)];}
  auto& Number(NativeAdmissionNumber field) noexcept{return numbers[std::size_t(field)];}
};
// Byte-borrowing accessors require a successful full decode. No native typed
// arrays alias potentially unaligned persisted bytes and no heap is needed.
class NativeStartupAdmissionEvidenceView {
 public:
  core::platform::Uuid Identity(NativeAdmissionIdentity field)const noexcept {
    core::platform::Uuid id;std::copy_n(bytes_.data()+16+16*std::size_t(field),16,id.bytes.begin());return id;
  }
  core::platform::u64 Number(NativeAdmissionNumber field)const noexcept {
    return core::platform::LoadLittle64(bytes_.data()+416+8*std::size_t(field));
  }
  NativeStartupPolicyOutcome outcome()const noexcept {
    return NativeStartupPolicyOutcome(core::platform::LoadLittle16(bytes_.data()+512)-1);
  }
  NativeAdmissionReason reason()const noexcept{return NativeAdmissionReason(core::platform::LoadLittle16(bytes_.data()+536));}
  core::platform::u16 cache_mask()const noexcept{return core::platform::LoadLittle16(bytes_.data()+514);}
  std::size_t count(unsigned section)const noexcept{return core::platform::LoadLittle32(bytes_.data()+520+4*section);}
  core::platform::Uuid Item(unsigned section,std::size_t index)const noexcept {
    std::size_t offset=640;for(unsigned s=0;s<section;++s)offset+=16*count(s);
    core::platform::Uuid id;std::copy_n(bytes_.data()+offset+16*index,16,id.bytes.begin());return id;
  }
  NativeStartupPolicyBinding Policy(std::size_t index)const noexcept {
    const auto* p=bytes_.data()+640+16*(count(0)+count(1)+count(2))+80*index;
    NativeStartupPolicyBinding b;
    std::copy_n(p,16,b.policy_uuid.bytes.begin());std::copy_n(p+16,16,b.descriptor_version_uuid.bytes.begin());
    std::copy_n(p+32,16,b.body_uuid.bytes.begin());
    using namespace core::platform;
    b.generation=LoadLittle64(p+48);b.body_generation=LoadLittle64(p+56);b.cache_invalidation_mask=LoadLittle64(p+64);
    b.decision=core::catalog::CatalogPolicyDefault(LoadLittle16(p+72));b.precedence_rank=LoadLittle16(p+74);
    b.scope=core::catalog::CatalogPolicyScope(LoadLittle16(p+76));b.body_resolved=p[78]!=0;return b;
  }
  std::span<const core::platform::byte> bytes()const noexcept{return bytes_;}
 private:
  friend struct NativeAdmissionEvidenceDecoder;
  std::span<const core::platform::byte> bytes_;
};
struct NativeStartupAdmissionEvidenceImage {
  NativeAdmissionEvidenceError error=NativeAdmissionEvidenceError::invalid_header;
  std::optional<NativeStartupAdmissionEvidenceView> record;
  std::span<const core::platform::byte> bytes;
  bool ok()const noexcept{return error==NativeAdmissionEvidenceError::none&&record.has_value();}
};
namespace native_admission_evidence_detail {
using core::platform::Uuid;using core::platform::byte;using core::platform::u64;
using E=NativeAdmissionEvidenceError;using I=NativeAdmissionIdentity;using N=NativeAdmissionNumber;
inline bool Zero(std::span<const byte> bytes)noexcept{return std::ranges::all_of(bytes,[](byte b){return b==0;});}
inline auto Digest(std::span<const byte> bytes)noexcept {
  const std::array<byte,32> zero{};
  const core::hash::HashDigestSegment parts[]={{bytes.data(),544},{zero.data(),zero.size()},{bytes.data()+576,bytes.size()-576}};
  return core::hash::ComputeSha256DigestPartsNative(parts,3);
}
inline int Specificity(core::catalog::CatalogPolicyScope scope)noexcept {
  using S=core::catalog::CatalogPolicyScope;
  return scope==S::transaction?3:scope==S::object||scope==S::filespace?2:scope==S::database?0:1;
}
inline bool Less(const NativeStartupPolicyBinding& a,const NativeStartupPolicyBinding& b)noexcept {
  if(a.precedence_rank!=b.precedence_rank)return a.precedence_rank<b.precedence_rank;
  if(Specificity(a.scope)!=Specificity(b.scope))return Specificity(a.scope)>Specificity(b.scope);
  return a.policy_uuid<b.policy_uuid;
}
inline E Semantics(const NativeStartupAdmissionEvidenceView& v)noexcept {
  using namespace core::platform;using S=core::catalog::CatalogPolicyScope;using D=core::catalog::CatalogPolicyDefault;
  const auto b=v.bytes();
  for(std::size_t i=0;i<25;++i){const auto id=v.Identity(I(i));
    if(i==24&&id.is_nil())continue;
    if(!core::uuid::IsEngineIdentityUuid(id))return E::invalid_identity;
    for(std::size_t j=0;j<i;++j)if(id==v.Identity(I(j))&&!(i==24&&(j==15||j==19)))return E::invalid_identity;
  }
  for(std::size_t n=0;n<12;++n)if(!v.Number(N(n)))return E::invalid_state;
  if(v.Number(N::transaction_number)<=1||v.Number(N::policy_time_utc_us)<v.Number(N::authorization_time_utc_us)||
      LoadLittle16(b.data()+512)<1||LoadLittle16(b.data()+512)>6||LoadLittle16(b.data()+514)>255||
      LoadLittle32(b.data()+516)!=7||LoadLittle16(b.data()+536)<1||LoadLittle16(b.data()+536)>4||
      Zero(b.subspan(576,32)))return E::invalid_state;
  const auto contains=[&](unsigned section,const Uuid& id){
    for(std::size_t n=0;n<v.count(section);++n)if(v.Item(section,n)==id)return true;return false;
  };
  for(unsigned section=0;section<3;++section){
    if(!v.count(section))return E::invalid_state;
    for(std::size_t n=0;n<v.count(section);++n){const auto id=v.Item(section,n);
      if(!core::uuid::IsEngineIdentityUuid(id)||(n&&!(v.Item(section,n-1)<id)))return E::invalid_identity;
    }
  }
  if(!contains(0,v.Identity(I::actor))||!contains(1,v.Identity(I::database))||
      !contains(1,v.Identity(I::storage_filespace)))return E::invalid_state;
  for(const auto id:{I::access_descriptor_version,I::access_body_version,I::evidence_descriptor_version,I::evidence_body_version})
    if(!contains(2,v.Identity(id)))return E::invalid_state;
  if(!v.count(3))return E::invalid_state;
  u64 generation=0,mask=0;NativeStartupPolicyOutcome outcome=NativeStartupPolicyOutcome::allow;
  NativeAdmissionReason reason=NativeAdmissionReason::startup_policy_allow;Uuid decisive;
  for(std::size_t n=0;n<v.count(3);++n){const auto p=v.Policy(n);
    if(!core::uuid::IsEngineIdentityUuid(p.policy_uuid)||!core::uuid::IsEngineIdentityUuid(p.descriptor_version_uuid)||
        !core::uuid::IsEngineIdentityUuid(p.body_uuid)||!p.generation||!p.body_generation||!p.precedence_rank||
        p.cache_invalidation_mask>255||u64(p.decision)<1||u64(p.decision)>5||
        (p.scope!=S::database&&p.scope!=S::user&&p.scope!=S::role&&p.scope!=S::group&&p.scope!=S::object&&
         p.scope!=S::filespace&&p.scope!=S::transaction)||!contains(2,p.descriptor_version_uuid))return E::invalid_state;
    if(n&&!Less(v.Policy(n-1),p))return E::invalid_state;
    for(std::size_t j=0;j<n;++j)if(v.Policy(j).policy_uuid==p.policy_uuid)return E::invalid_identity;
    generation=std::max(generation,p.generation);mask|=p.cache_invalidation_mask;
    if(outcome==NativeStartupPolicyOutcome::allow){
      if(p.decision!=D::allow){decisive=p.policy_uuid;reason=NativeAdmissionReason::applicable_policy_refusal;
        outcome=p.decision==D::defer?NativeStartupPolicyOutcome::defer:p.decision==D::require_human_review?
            NativeStartupPolicyOutcome::require_human_review:NativeStartupPolicyOutcome::deny;
      }else if(!p.body_resolved){decisive=p.policy_uuid;reason=NativeAdmissionReason::unresolved_applicable_body;outcome=NativeStartupPolicyOutcome::deny;}
    }
  }
  if(generation!=v.Number(N::effective_policy_generation)||mask!=v.cache_mask()||decisive!=v.Identity(I::decisive_policy))return E::invalid_state;
  if(v.outcome()==NativeStartupPolicyOutcome::require_revalidation&&outcome==NativeStartupPolicyOutcome::allow)
    return v.reason()==NativeAdmissionReason::policy_generation_changed?E::none:E::invalid_state;
  return outcome==v.outcome()&&reason==v.reason()?E::none:E::invalid_state;
}
}
struct NativeAdmissionEvidenceDecoder {
 static NativeStartupAdmissionEvidenceImage Decode(std::span<const core::platform::byte> b,core::platform::u64 budget)noexcept {
  using namespace native_admission_evidence_detail;using namespace core::platform;
  const auto fail=[](E e){return NativeStartupAdmissionEvidenceImage{e,{},{}};};
  if(!storage::disk::detail::DecodedRange(b).valid)return fail(E::invalid_workspace);
  if(b.size()>budget||b.size()>kNativeAdmissionEvidenceMaximumBytes)return fail(E::resource_exhausted);
  if(b.size()<640||!std::equal(b.begin(),b.begin()+8,"SBSA0001")||LoadLittle16(b.data()+8)!=1||
      LoadLittle16(b.data()+10)!=640||LoadLittle32(b.data()+12)!=b.size()||!Zero(b.subspan(538,6))||!Zero(b.subspan(608,32)))
    return fail(E::invalid_header);
  NativeStartupAdmissionEvidenceView v;v.bytes_=b;
  u64 expected=640;
  for(unsigned s=0;s<4;++s){if(v.count(s)>128)return fail(E::resource_exhausted);expected+=(s==3?80:16)*v.count(s);}
  if(expected!=b.size())return fail(E::invalid_header);
  const auto policy_offset=640+16*(v.count(0)+v.count(1)+v.count(2));
  for(std::size_t n=0;n<v.count(3);++n)if(b[policy_offset+80*n+78]>1||b[policy_offset+80*n+79])return fail(E::invalid_header);
  const auto error=Semantics(v);if(error!=E::none)return fail(error);
  const auto digest=Digest(b);if(!digest.ok())return fail(E::hash_failure);
  if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+544))return fail(E::invalid_integrity);
  return {E::none,v,b};
 }
};
inline auto DecodeNativeStartupAdmissionEvidence(std::span<const core::platform::byte> b,core::platform::u64 budget)noexcept {
  return NativeAdmissionEvidenceDecoder::Decode(b,budget);
}
inline NativeStartupAdmissionEvidenceImage EncodeNativeStartupAdmissionEvidenceInto(
    const NativeStartupAdmissionEvidenceInput& input,core::platform::u64 budget,std::span<core::platform::byte> output)noexcept {
  using namespace native_admission_evidence_detail;using namespace core::platform;
  const auto fail=[](E e){return NativeStartupAdmissionEvidenceImage{e,{},{}};};
  if(!storage::disk::detail::DisjointNativeDecodeRegions(output,std::span{&input,1},input.subjects,input.targets,input.metadata_versions,input.policies))
    return fail(E::invalid_workspace);
  for(auto values:{input.subjects,input.targets,input.metadata_versions})
    if(!storage::disk::detail::DecodedRange(values).valid||reinterpret_cast<std::uintptr_t>(values.data())%alignof(Uuid))return fail(E::invalid_workspace);
  if(!storage::disk::detail::DecodedRange(input.policies).valid||reinterpret_cast<std::uintptr_t>(input.policies.data())%alignof(NativeStartupPolicyBinding))return fail(E::invalid_workspace);
  if(input.subjects.size()>128||input.targets.size()>128||input.metadata_versions.size()>128||input.policies.size()>128)return fail(E::resource_exhausted);
  if(u64(input.outcome)>5||u16(input.reason)<1||u16(input.reason)>4)return fail(E::invalid_state);
  for(const auto& p:input.policies)if(u64(p.decision)<1||u64(p.decision)>5||u64(p.scope)>21)return fail(E::invalid_state);
  const auto length=640+16*(input.subjects.size()+input.targets.size()+input.metadata_versions.size())+80*input.policies.size();
  if(length>budget||length>output.size())return fail(E::resource_exhausted);
  auto b=output.first(length);std::fill(b.begin(),b.end(),0);std::copy_n("SBSA0001",8,b.begin());
  StoreLittle16(b.data()+8,1);StoreLittle16(b.data()+10,640);StoreLittle32(b.data()+12,length);
  for(std::size_t n=0;n<25;++n)std::copy(input.identities[n].bytes.begin(),input.identities[n].bytes.end(),b.begin()+16+16*n);
  for(std::size_t n=0;n<12;++n)StoreLittle64(b.data()+416+8*n,input.numbers[n]);
  StoreLittle16(b.data()+512,u16(input.outcome)+1);StoreLittle32(b.data()+516,7);
  StoreLittle16(b.data()+536,u16(input.reason));std::copy(input.source_checkpoint_sha256.begin(),input.source_checkpoint_sha256.end(),b.begin()+576);
  std::size_t at=640;unsigned section=0;u64 mask=0;
  for(auto values:{input.subjects,input.targets,input.metadata_versions}){StoreLittle32(b.data()+520+4*section++,values.size());
    for(const auto& id:values){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);at+=16;}}
  StoreLittle32(b.data()+532,input.policies.size());
  for(const auto& p:input.policies){auto* out=b.data()+at;at+=80;
    std::copy(p.policy_uuid.bytes.begin(),p.policy_uuid.bytes.end(),out);std::copy(p.descriptor_version_uuid.bytes.begin(),p.descriptor_version_uuid.bytes.end(),out+16);
    std::copy(p.body_uuid.bytes.begin(),p.body_uuid.bytes.end(),out+32);StoreLittle64(out+48,p.generation);StoreLittle64(out+56,p.body_generation);
    StoreLittle64(out+64,p.cache_invalidation_mask);StoreLittle16(out+72,u16(p.decision));StoreLittle16(out+74,p.precedence_rank);
    StoreLittle16(out+76,u16(p.scope));out[78]=p.body_resolved?1:0;mask|=p.cache_invalidation_mask;
  }
  if(mask>255)return fail(E::invalid_state);StoreLittle16(b.data()+514,mask);
  const auto digest=Digest(b);if(!digest.ok())return fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+544);
  return DecodeNativeStartupAdmissionEvidence(b,budget);
}
} // namespace scratchbird::engine::internal_api
