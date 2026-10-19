// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_bootstrap_policy.hpp"
#include "catalog_transaction_policy.hpp"
#include "catalog_isolation_profile.hpp"
#include "catalog_lock_policy.hpp"
#include "catalog_evidence_policy.hpp"
#include "uuid.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeTransactionPolicyError {
  none,source_required,workspace_required,invalid_request,duplicate_identity,
  missing_pack,invalid_pack,missing_selection,ambiguous_selection,invalid_selection,
  missing_descriptor,invalid_descriptor,unsupported_scope,missing_body,invalid_body,
  body_binding,missing_isolation,invalid_isolation,unsupported_reference,evidence_binding,
  missing_filespace,origin_not_committed
};
struct NativeTransactionPolicyRequest {
  core::platform::Uuid policy_pack_uuid;
  core::platform::u64 policy_pack_generation=0;
  bool operator==(const NativeTransactionPolicyRequest&) const=default;
};
struct NativeResolvedLockPolicy {
  core::catalog::CatalogPolicyDescriptor descriptor;
  core::catalog::CatalogLockPolicyBody body;
  core::platform::Uuid descriptor_version_uuid,body_version_uuid;
};
struct NativeTransactionPolicyDeclarations {
  NativeTransactionPolicyError error=NativeTransactionPolicyError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  NativeTransactionPolicyRequest request;
  core::catalog::CatalogBootstrapPolicyProfile selection_profile;
  core::platform::Uuid selection_version_uuid;
  core::catalog::CatalogPolicyDescriptor descriptor;
  core::catalog::CatalogTransactionPolicyBody body;
  core::platform::Uuid descriptor_version_uuid,body_version_uuid;
  core::catalog::CatalogIsolationProfile default_isolation,startup_isolation;
  core::platform::Uuid default_isolation_version_uuid,startup_isolation_version_uuid;
  // Transaction policy, startup wait policy, startup deadlock policy.
  std::array<NativeResolvedLockPolicy,3> locks;
  core::catalog::CatalogPolicyDescriptor evidence_descriptor;
  core::catalog::CatalogEvidencePolicyBody evidence;
  core::platform::Uuid evidence_descriptor_version_uuid,evidence_body_version_uuid;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok()const noexcept{return error==NativeTransactionPolicyError::none&&selection.has_value();}
};
// Complete declaration closure from real selected rows. The host retains the
// source, including borrowed profile/evidence text, until this observation ends.
// No caller-supplied row array or policy label constructs an active transaction.
inline NativeTransactionPolicyDeclarations ResolveNativeTransactionPolicyDeclarations(
    const storage::database::NativeCatalogCapture& capture,
    const NativeTransactionPolicyRequest& input,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;namespace mga=transaction::mga;using E=NativeTransactionPolicyError;
  using core::platform::Uuid;using core::platform::u64;using Row=storage::database::NativeCapturedCatalogRow;
  const auto fail=[](E error,Uuid object={},Uuid reference={}){
    NativeTransactionPolicyDeclarations r;r.error=error;r.object_uuid=object;r.referenced_uuid=reference;return r;};
  if(!capture.ok()||!capture.inventory())return fail(E::source_required);
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      reinterpret_cast<std::uintptr_t>(scratch.data())%alignof(std::size_t)||
      !capture.DisjointWorkspace(std::as_bytes(scratch))||
      !storage::disk::detail::DisjointNativeDecodeRegions(scratch,std::span{&input,1}))return fail(E::workspace_required);
  if(!core::uuid::IsEngineIdentityUuid(input.policy_pack_uuid)||!input.policy_pack_generation)return fail(E::invalid_request);
  const auto request=input;auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const Uuid& id)->const Row*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?nullptr:&rows[*at];
  };
  const auto database=capture.selection()->header.database_uuid;
  const auto inventory=capture.inventory()->inventory;
  const auto active=[](const Row& r){return r.role==5&&!r.row.provisional&&!r.row.name_payload&&
      core::uuid::IsEngineIdentityUuid(r.row.version_uuid)&&!r.row.metadata.record.header.deleted&&
      r.row.effective_lifecycle==c::CatalogObjectLifecycle::active&&r.row.effective_status==c::CatalogObjectStatus::active;};
  const auto origin=[&](const auto& value){
    const auto at=std::lower_bound(inventory.entries.begin(),inventory.entries.end(),value.origin_local_transaction_id,
        [](const auto& entry,u64 n){return entry.identity.local_id.value<n;});
    return at!=inventory.entries.end()&&at->identity.local_id.value==value.origin_local_transaction_id&&
      at->identity.transaction_uuid.value==value.origin_transaction_uuid.value&&at->identity.scope==mga::TransactionScope::local_node&&
      mga::HasCommittedInventoryOutcome(*at);
  };
  const auto* pack_row=find(request.policy_pack_uuid);if(!pack_row)return fail(E::missing_pack,request.policy_pack_uuid);
  const auto pack=c::DecodeCatalogBootstrapPolicyPack(pack_row->row.metadata.record.payload);
  if(!active(*pack_row)||!c::CatalogBootstrapPolicyMatchesMetadata(pack_row->row.metadata)||!pack.ok()||
      pack.record->generation!=request.policy_pack_generation)return fail(E::invalid_pack,request.policy_pack_uuid);
  if(!origin(*pack.record))return fail(E::origin_not_committed,request.policy_pack_uuid);
  NativeTransactionPolicyDeclarations out;
  for(const auto& row:rows){const auto& m=row.row.metadata;
    if(m.record.header.deleted||m.object_subtype!="bootstrap_policy_profile")continue;
    const auto profile=c::DecodeCatalogBootstrapPolicyProfile(m.record.payload);
    if(!profile.ok())return fail(E::invalid_selection,m.record.header.object_uuid.value);
    const auto& p=*profile.record;
    if(p.policy_pack_uuid!=request.policy_pack_uuid||p.area!="transaction_mga_cleanup_archive_backup_forward")continue;
    if(!out.selection_version_uuid.is_nil())return fail(E::ambiguous_selection,p.profile_uuid);
    if(!active(row)||!c::CatalogBootstrapPolicyMatchesMetadata(m)||p.policy_pack_generation!=request.policy_pack_generation||
        p.mode!="mga_inventory_authority"||!p.resource_definition_uuid||!p.resource_definition_generation)
      return fail(E::invalid_selection,p.profile_uuid);
    if(!origin(p))return fail(E::origin_not_committed,p.profile_uuid);
    out.selection_profile=p;out.selection_version_uuid=row.row.version_uuid;
  }
  if(out.selection_version_uuid.is_nil())return fail(E::missing_selection,request.policy_pack_uuid);
  const auto descriptor=[&](Uuid id,c::CatalogPolicyClass kind,c::CatalogPolicyDescriptor& value,Uuid& version)->E{
    const auto* row=find(id);if(!row)return E::missing_descriptor;
    if(!active(*row)||!c::IsCatalogPolicyDescriptorPayload(row->row.metadata.record.payload)||
        !c::CatalogPolicyDescriptorMatchesDatabase(row->row.metadata,database))return E::invalid_descriptor;
    const auto decoded=c::DecodeCatalogPolicyDescriptor(row->row.metadata.record.payload);
    if(!decoded.ok()||decoded.record->policy_class!=kind)return E::invalid_descriptor;
    value=*decoded.record;
    if(value.scope!=c::CatalogPolicyScope::database||value.scope_uuid!=database||value.cluster_uuid)return E::unsupported_scope;
    if(value.disclosure_profile_uuid||value.retention_profile_uuid)return E::unsupported_reference;
    if(!origin(value))return E::origin_not_committed;
    version=row->row.version_uuid;return E::none;
  };
  const auto policy_id=*out.selection_profile.resource_definition_uuid;
  auto error=descriptor(policy_id,c::CatalogPolicyClass::transaction,out.descriptor,out.descriptor_version_uuid);
  if(error!=E::none)return fail(error,policy_id);
  if(out.descriptor.generation!=*out.selection_profile.resource_definition_generation)return fail(E::body_binding,out.selection_profile.profile_uuid,policy_id);
  const auto* body_row=find(out.descriptor.body_uuid);if(!body_row)return fail(E::missing_body,policy_id,out.descriptor.body_uuid);
  const auto body=c::DecodeCatalogTransactionPolicy(body_row->row.metadata.record.payload);
  if(!active(*body_row)||!c::CatalogTransactionPolicyMatchesDatabase(body_row->row.metadata,database)||!body.ok())
    return fail(E::invalid_body,policy_id,out.descriptor.body_uuid);
  if(!c::CatalogTransactionPolicyMatchesDescriptor(*body.record,out.descriptor))return fail(E::body_binding,policy_id,out.descriptor.body_uuid);
  if(!origin(*body.record))return fail(E::origin_not_committed,out.descriptor.body_uuid);
  out.body=*body.record;out.body_version_uuid=body_row->row.version_uuid;
  if(out.descriptor.evidence_profile_uuid&&*out.descriptor.evidence_profile_uuid!=out.body.evidence_policy_uuid)
    return fail(E::evidence_binding,policy_id,*out.descriptor.evidence_profile_uuid);
  const auto lock=[&](Uuid id,NativeResolvedLockPolicy& result)->E{
    auto e=descriptor(id,c::CatalogPolicyClass::lock_admission,result.descriptor,result.descriptor_version_uuid);
    if(e!=E::none)return e;const auto* row=find(result.descriptor.body_uuid);if(!row)return E::missing_body;
    const auto value=c::DecodeCatalogLockPolicy(row->row.metadata.record.payload);
    if(!active(*row)||!c::CatalogLockPolicyMatchesDatabase(row->row.metadata,database)||!value.ok())return E::invalid_body;
    if(!c::CatalogLockPolicyMatchesDescriptor(*value.record,result.descriptor))return E::body_binding;
    if(!origin(*value.record))return E::origin_not_committed;
    // Those owners need their real mapping/window/cluster resolvers. Refusing
    // an unsupported reference never means fallback to a local no-wait mode.
    if(value.record->donor_mapping_uuid||value.record->maintenance_window_policy_uuid||value.record->cluster_uuid)return E::unsupported_reference;
    if(value.record->evidence_policy_uuid!=out.body.evidence_policy_uuid||
        (result.descriptor.evidence_profile_uuid&&*result.descriptor.evidence_profile_uuid!=out.body.evidence_policy_uuid))return E::evidence_binding;
    result.body=*value.record;result.body_version_uuid=row->row.version_uuid;return E::none;
  };
  error=lock(out.body.lock_policy_uuid,out.locks[0]);if(error!=E::none)return fail(error,policy_id,out.body.lock_policy_uuid);
  for(std::size_t i=0;i<out.body.allowed_isolation_profile_count;++i){const auto id=out.body.allowed_isolation_profiles[i];
    const auto* row=find(id);if(!row)return fail(E::missing_isolation,policy_id,id);
    const auto profile=c::DecodeCatalogIsolationProfile(row->row.metadata.record.payload);
    if(!active(*row)||!c::CatalogIsolationProfileMatchesDatabase(row->row.metadata,database)||!profile.ok())return fail(E::invalid_isolation,policy_id,id);
    if(!origin(*profile.record))return fail(E::origin_not_committed,id);
    if(profile.record->donor_mapping_uuid||profile.record->cluster_uuid)return fail(E::unsupported_reference,policy_id,id);
    std::array<NativeResolvedLockPolicy,2> related;
    error=lock(profile.record->wait_policy_uuid,related[0]);if(error!=E::none)return fail(error,id,profile.record->wait_policy_uuid);
    error=lock(profile.record->deadlock_policy_uuid,related[1]);if(error!=E::none)return fail(error,id,profile.record->deadlock_policy_uuid);
    if(id==out.body.default_isolation_profile_uuid){out.default_isolation=*profile.record;out.default_isolation_version_uuid=row->row.version_uuid;}
    if(id==out.body.startup_isolation_profile_uuid){out.startup_isolation=*profile.record;out.startup_isolation_version_uuid=row->row.version_uuid;
      out.locks[1]=related[0];out.locks[2]=related[1];}
  }
  const auto evidence_id=out.body.evidence_policy_uuid;
  error=descriptor(evidence_id,c::CatalogPolicyClass::evidence,out.evidence_descriptor,out.evidence_descriptor_version_uuid);
  if(error!=E::none)return fail(error,policy_id,evidence_id);
  if(out.evidence_descriptor.evidence_profile_uuid&&*out.evidence_descriptor.evidence_profile_uuid!=evidence_id)
    return fail(E::unsupported_reference,evidence_id,*out.evidence_descriptor.evidence_profile_uuid);
  const auto* evidence_row=find(out.evidence_descriptor.body_uuid);if(!evidence_row)return fail(E::missing_body,evidence_id,out.evidence_descriptor.body_uuid);
  const auto evidence=c::DecodeCatalogEvidencePolicy(evidence_row->row.metadata.record.payload);
  if(!active(*evidence_row)||!c::CatalogEvidencePolicyMatchesDatabase(evidence_row->row.metadata,database)||!evidence.ok())
    return fail(E::invalid_body,evidence_id,out.evidence_descriptor.body_uuid);
  if(!c::CatalogEvidencePolicyMatchesDescriptor(*evidence.record,out.evidence_descriptor))return fail(E::body_binding,evidence_id,out.evidence_descriptor.body_uuid);
  if(!origin(*evidence.record))return fail(E::origin_not_committed,out.evidence_descriptor.body_uuid);
  const auto storage=std::find_if(capture.filespaces().begin(),capture.filespaces().end(),[&](const auto& member){
    return member.bootstrap.filespace_uuid==evidence.record->storage_filespace_uuid&&member.bootstrap.database_uuid==database;});
  if(storage==capture.filespaces().end())return fail(E::missing_filespace,evidence_id,evidence.record->storage_filespace_uuid);
  out.evidence=*evidence.record;out.evidence_body_version_uuid=evidence_row->row.version_uuid;
  out.request=request;out.selection=capture.selection();out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
