// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_verification_operation.hpp"
#include "native_catalog_cohort_capture.hpp"
#include "catalog_verification_policy.hpp"
#include "catalog_evidence_policy.hpp"
#include "catalog_authorization_policy.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_database_record_codec.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
struct NativeVerificationPolicyRequest {
  Uuid repair_policy_uuid,quarantine_policy_uuid,salvage_policy_uuid,evidence_policy_uuid;
  NativeVerificationClass verification_class=NativeVerificationClass::startup_full;
  NativeVerificationAuthority authority_class=NativeVerificationAuthority::startup_recovery;
  u32 verification_levels=0;
  bool operator==(const NativeVerificationPolicyRequest&) const=default;
};
enum class NativeVerificationPolicyError {
  none,source_required,invalid_request,workspace_required,duplicate_identity,
  provisional_row,missing_policy,invalid_descriptor,unsupported_scope,
  missing_body,invalid_body,body_binding,verification_class_refused,
  authority_class_refused,verification_level_refused,evidence_binding,
  missing_filespace,access_policy_binding,access_schema_binding
};
struct NativeVerificationPolicyDeclarations {
  NativeVerificationPolicyError error=NativeVerificationPolicyError::source_required;
  Uuid object_uuid,referenced_uuid;
  NativeVerificationPolicyRequest request;
  std::array<core::catalog::CatalogPolicyDescriptor,3> descriptors;
  std::array<core::catalog::CatalogVerificationPolicyBody,3> bodies;
  core::catalog::CatalogPolicyDescriptor evidence_descriptor,access_descriptor;
  core::catalog::CatalogEvidencePolicyBody evidence;
  core::catalog::CatalogAuthorizationPolicyBody access_body;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok() const noexcept{return error==NativeVerificationPolicyError::none&&selection.has_value();}
};
// Resolve declarations from the actual captured, MGA-visible catalog. Evidence
// text borrows the capture. The owner must retain it and the original source.
// This is not activation or a permission: committed activation, actual grants,
// row/column policy evaluation and evidence persistence remain gates
// of the owning verification operation. No UUID/name/pack label supplies them.
inline NativeVerificationPolicyDeclarations ResolveNativeVerificationPolicyDeclarations(
    const storage::database::NativeCatalogCapture& capture,
    const NativeVerificationPolicyRequest& input,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeVerificationPolicyError;
  NativeVerificationPolicyDeclarations out;
  const auto fail=[](E error,Uuid object=Uuid{},Uuid reference=Uuid{}){
    NativeVerificationPolicyDeclarations refused;
    refused.error=error;refused.object_uuid=object;refused.referenced_uuid=reference;return refused;
  };
  if(!capture.ok())return out;
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!native_verification_state_detail::Region(scratch)||
      !capture.DisjointWorkspace(std::as_bytes(scratch))||
      !storage::disk::detail::DisjointNativeDecodeRegions(scratch,std::span{&input,1}))
    return fail(E::workspace_required);
  const auto request=input;
  const std::array selected{request.repair_policy_uuid,request.quarantine_policy_uuid,
      request.salvage_policy_uuid,request.evidence_policy_uuid};
  for(std::size_t i=0;i<selected.size();++i){
    if(!core::uuid::IsEngineIdentityUuid(selected[i]))return fail(E::invalid_request,selected[i]);
    for(std::size_t j=0;j<i;++j)if(selected[i]==selected[j])return fail(E::invalid_request,selected[i]);
  }
  if(u16(request.verification_class)<1||u16(request.verification_class)>9||
      u16(request.authority_class)<1||u16(request.authority_class)>6||
      !request.verification_levels||(request.verification_levels&~u32{1023}))return fail(E::invalid_request);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const Uuid& id)->const storage::database::NativeCapturedCatalogRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?nullptr:&rows[*at];
  };
  const auto database=capture.selection()->header.database_uuid;
  const auto active=[](const storage::database::NativeCapturedCatalogRow& row){
    return row.row.effective_lifecycle==c::CatalogObjectLifecycle::active&&
        row.row.effective_status==c::CatalogObjectStatus::active;
  };
  const auto descriptor=[&](Uuid id,c::CatalogPolicyClass kind,c::CatalogPolicyDescriptor& value)->E{
    const auto* found=find(id);if(!found)return E::missing_policy;
    const auto& row=found->row;if(row.provisional)return E::provisional_row;
    if(!active(*found)||found->role!=5||row.name_payload||!c::CatalogPolicyDescriptorMatchesDatabase(row.metadata,database)||
        !c::IsCatalogPolicyDescriptorPayload(row.metadata.record.payload))return E::invalid_descriptor;
    const auto decoded=c::DecodeCatalogPolicyDescriptor(row.metadata.record.payload);
    if(!decoded.ok()||decoded.record->policy_class!=kind)return E::invalid_descriptor;
    value=*decoded.record;
    // The database startup verifier does not widen an object/schema/cluster
    // policy into database authority. Those owners need their own resolver.
    if(value.scope!=c::CatalogPolicyScope::database||value.scope_uuid!=database||value.cluster_uuid)
      return E::unsupported_scope;
    return E::none;
  };
  for(std::size_t i=0;i<out.bodies.size();++i){
    auto& policy=out.descriptors[i];const auto error=descriptor(selected[i],c::CatalogPolicyClass::verification_repair,policy);
    if(error!=E::none)return fail(error,selected[i]);
    const auto* found=find(policy.body_uuid);if(!found)return fail(E::missing_body,selected[i],policy.body_uuid);
    const auto& row=found->row;if(row.provisional)return fail(E::provisional_row,policy.body_uuid);
    if(!active(*found)||found->role!=5||row.name_payload||!c::IsCatalogVerificationPolicyPayload(row.metadata.record.payload)||
        !c::CatalogVerificationPolicyMatchesDatabase(row.metadata,database))return fail(E::invalid_body,selected[i],policy.body_uuid);
    const auto decoded=c::DecodeCatalogVerificationPolicy(row.metadata.record.payload);
    if(!decoded.ok()||!c::CatalogVerificationPolicyMatchesDescriptor(*decoded.record,policy,c::VerificationPolicyRole(i+1)))
      return fail(E::body_binding,selected[i],policy.body_uuid);
    const auto& body=*decoded.record;
    if(!(body.verification_class_mask&(u64{1}<<(u16(request.verification_class)-1))))return fail(E::verification_class_refused,selected[i]);
    if(!(body.authority_class_mask&(u64{1}<<(u16(request.authority_class)-1))))return fail(E::authority_class_refused,selected[i]);
    if((body.verification_level_mask&request.verification_levels)!=request.verification_levels)return fail(E::verification_level_refused,selected[i]);
    if(body.evidence_policy_uuid!=request.evidence_policy_uuid||
        (policy.evidence_profile_uuid&&*policy.evidence_profile_uuid!=request.evidence_policy_uuid))
      return fail(E::evidence_binding,selected[i],body.evidence_policy_uuid);
    out.bodies[i]=body;
  }
  const auto evidence_error=descriptor(request.evidence_policy_uuid,c::CatalogPolicyClass::evidence,out.evidence_descriptor);
  if(evidence_error!=E::none)return fail(evidence_error,request.evidence_policy_uuid);
  const auto evidence_id=out.evidence_descriptor.body_uuid;const auto* evidence=find(evidence_id);
  if(!evidence)return fail(E::missing_body,request.evidence_policy_uuid,evidence_id);
  if(evidence->row.provisional)return fail(E::provisional_row,evidence_id);
  const auto& metadata=evidence->row.metadata;
  if(!active(*evidence)||evidence->role!=5||evidence->row.name_payload||!c::IsCatalogEvidencePolicyPayload(metadata.record.payload)||
      !c::CatalogEvidencePolicyMatchesDatabase(metadata,database))return fail(E::invalid_body,request.evidence_policy_uuid,evidence_id);
  const auto body=c::DecodeCatalogEvidencePolicy(metadata.record.payload);
  if(!body.ok()||!c::CatalogEvidencePolicyMatchesDescriptor(*body.record,out.evidence_descriptor))
    return fail(E::body_binding,request.evidence_policy_uuid,evidence_id);
  out.evidence=*body.record;
  const auto storage=std::find_if(capture.filespaces().begin(),capture.filespaces().end(),[&](const auto& member){
    return member.bootstrap.filespace_uuid==out.evidence.storage_filespace_uuid&&member.bootstrap.database_uuid==database;
  });
  if(storage==capture.filespaces().end())return fail(E::missing_filespace,evidence_id,out.evidence.storage_filespace_uuid);
  const auto access=out.bodies[1].access_policy_uuid;
  if(!access||descriptor(*access,c::CatalogPolicyClass::authorization,out.access_descriptor)!=E::none)
    return fail(E::access_policy_binding,selected[1],access.value_or(Uuid{}));
  const auto access_body_id=out.access_descriptor.body_uuid;const auto* access_row=find(access_body_id);
  if(!access_row)return fail(E::missing_body,*access,access_body_id);
  if(access_row->row.provisional)return fail(E::provisional_row,access_body_id);
  const auto& access_metadata=access_row->row.metadata;
  if(!active(*access_row)||access_row->role!=5||access_row->row.name_payload||!c::IsCatalogAuthorizationPolicyPayload(access_metadata.record.payload)||
      !c::CatalogAuthorizationPolicyMatchesDatabase(access_metadata,database))return fail(E::invalid_body,*access,access_body_id);
  const auto access_body=c::DecodeCatalogAuthorizationPolicy(access_metadata.record.payload);
  if(!access_body.ok()||!c::CatalogAuthorizationPolicyMatchesDescriptor(*access_body.record,out.access_descriptor))
    return fail(E::body_binding,*access,access_body_id);
  out.access_body=*access_body.record;
  const auto& a=out.access_body;
  const std::array schemas{a.user_schema_uuid,a.role_schema_uuid,a.group_schema_uuid,a.grant_schema_uuid,
      a.row_policy_schema_uuid,a.column_policy_schema_uuid,a.external_mapping_schema_uuid.value_or(Uuid{})};
  const auto schema_count=a.external_mapping_schema_uuid?7u:6u;
  for(unsigned i=0;i<schema_count;++i){
    const auto* schema_row=find(schemas[i]);
    if(!schema_row||!active(*schema_row)||schema_row->role!=1||schema_row->row.provisional||schema_row->row.name_payload||
        schema_row->row.metadata.record.header.kind!=c::CatalogRecordKind::schema||
        !c::CatalogSchemaDefinitionMatchesMetadata(schema_row->row.metadata))
      return fail(E::access_schema_binding,access_body_id,schemas[i]);
    const auto schema=c::DecodeCatalogSchemaDefinition(schema_row->row.metadata.record.payload);
    if(!schema.ok())return fail(E::access_schema_binding,access_body_id,schemas[i]);
    const auto* node=find(schema.definition->database_catalog_object_uuid.value);
    if(!node||!active(*node)||node->role!=1||node->row.provisional||node->row.name_payload||
        node->row.metadata.record.header.kind!=c::CatalogRecordKind::database)
      return fail(E::access_schema_binding,access_body_id,schemas[i]);
    const auto declared=c::DecodeCatalogDatabaseRecord(node->row.metadata.record.payload);
    if(!declared.ok()||declared.record->database_uuid.value!=database)
      return fail(E::access_schema_binding,access_body_id,schemas[i]);
  }
  out.request=request;out.selection=capture.selection();out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
