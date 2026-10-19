// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_bootstrap_policy.hpp"
#include "catalog_security_record_codec.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogSecurityReferenceError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,
  missing_owner,invalid_owner,missing_policy,invalid_policy,missing_pack,invalid_pack,
  name_policy_binding
};
struct NativeCatalogSecurityReferenceVerification {
  NativeCatalogSecurityReferenceError error=NativeCatalogSecurityReferenceError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  std::size_t owners=0,policies=0,pack_references=0;
  bool ok()const noexcept{return error==NativeCatalogSecurityReferenceError::none;}
};
// First-create common reference closure over the actual captured catalog. An
// owner must resolve to a native principal, including an intentionally disabled
// principal that still owns objects. Ownership is not permission to execute.
// The bootstrap security-policy family must resolve to its actual posture and
// exact pack generation. Policy evaluation, activation and external-authority
// admission remain their owning operations; UUIDs alone cannot supply them.
inline NativeCatalogSecurityReferenceVerification VerifyNativeCatalogSecurityReferences(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeCatalogSecurityReferenceError;using K=c::CatalogRecordKind;
  NativeCatalogSecurityReferenceVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.owners=out.policies=out.pack_references=0;return out;
  };
  if(!capture.ok())return out;
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      !capture.DisjointWorkspace(std::as_bytes(scratch)))return fail(E::workspace_required);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))
    return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const core::platform::Uuid& id)->const storage::database::NativeCapturedCatalogRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?nullptr:&rows[*at];
  };
  for(const auto& captured:rows){
    const auto& row=captured.row;const auto& m=row.metadata;const auto id=m.record.header.object_uuid.value;
    if(row.provisional)return fail(E::provisional_row,id);
    if(m.record.header.deleted)continue;
    const auto* owner=find(m.owner_uuid.value);
    if(!owner)return fail(E::missing_owner,id,m.owner_uuid.value);
    const auto& principal=owner->row.metadata;const auto kind=principal.record.header.kind;
    if(m.owner_uuid.kind!=core::platform::UuidKind::principal||owner->role!=4||owner->row.provisional||
        (kind!=K::user_account&&kind!=K::role_account&&kind!=K::group_account)||
        !c::CatalogSecurityPayloadMatchesMetadata(principal))return fail(E::invalid_owner,id,m.owner_uuid.value);
    ++out.owners;
    const auto* policy=find(m.security_policy_uuid.value);
    if(!policy)return fail(E::missing_policy,id,m.security_policy_uuid.value);
    const auto& definition=policy->row.metadata;
    if(m.security_policy_uuid.kind!=core::platform::UuidKind::object||policy->role!=5||policy->row.provisional||
        definition.record.header.kind!=K::policy||definition.object_subtype!="bootstrap_policy_profile"||
        !c::CatalogBootstrapPolicyMatchesMetadata(definition))return fail(E::invalid_policy,id,m.security_policy_uuid.value);
    const auto posture=c::DecodeCatalogBootstrapPolicyProfile(definition.record.payload);
    if(!posture.ok()||posture.record->profile_uuid!=m.security_policy_uuid.value||posture.record->area!="default_security_posture")
      return fail(E::invalid_policy,id,m.security_policy_uuid.value);
    ++out.policies;
    const auto* pack=find(posture.record->policy_pack_uuid);
    if(!pack)return fail(E::missing_pack,id,posture.record->policy_pack_uuid);
    const auto& pack_metadata=pack->row.metadata;
    if(pack->role!=5||pack->row.provisional||pack_metadata.record.header.kind!=K::policy||
        pack_metadata.object_subtype!="bootstrap_policy_pack"||!c::CatalogBootstrapPolicyMatchesMetadata(pack_metadata))
      return fail(E::invalid_pack,id,posture.record->policy_pack_uuid);
    const auto decoded=c::DecodeCatalogBootstrapPolicyPack(pack_metadata.record.payload);
    if(!decoded.ok()||decoded.record->policy_pack_uuid!=posture.record->policy_pack_uuid||
        decoded.record->generation!=posture.record->policy_pack_generation)
      return fail(E::invalid_pack,id,posture.record->policy_pack_uuid);
    ++out.pack_references;
    if(row.name_payload){
      const auto same=std::visit([&](const auto& name){return name.security_policy_uuid.kind==m.security_policy_uuid.kind&&
          name.security_policy_uuid.value==m.security_policy_uuid.value;},*row.name_payload);
      if(!same)return fail(E::name_policy_binding,id,m.security_policy_uuid.value);
    }
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
