// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_construction_intent.hpp"
#include "catalog_metadata_change.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogAuditError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,
  missing_event,invalid_event,event_binding,unsupported_event
};
struct NativeCatalogAuditVerification {
  NativeCatalogAuditError error=NativeCatalogAuditError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  std::size_t events=0,references=0;
  bool ok() const noexcept{return error==NativeCatalogAuditError::none;}
};
// Resolve common audit references against the actual selected catalog. The
// registered construction event attests intent only. Its retained presence is
// not a policy decision, authentication, completed effect or readiness receipt.
// Other event families require their own typed validation before admission.
// SB_AUDIT_CATALOG_METADATA_CHANGE (owner Q31 = 1, Q50 = 1): the registered
// catalog.metadata_change event is admitted too: a definition row version cites
// an event whose origin transaction created it (the construction intent for the
// create's own rows, a metadata-change event for a later transaction's). Name
// rows (role 2, the UUID-to-name lookup table) are data (Q50): their reference
// must resolve to a registered event, but their creator is not bound to it.
inline NativeCatalogAuditVerification VerifyNativeCatalogAuditReferences(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeCatalogAuditError;
  NativeCatalogAuditVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.events=out.references=0;return out;
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
    const bool change_event=m.object_subtype==scratchbird::core::catalog::CatalogObjectSubtype::catalog_metadata_change||c::IsCatalogMetadataChangePayload(m.record.payload);
    if(m.record.header.kind==c::CatalogRecordKind::audit_evidence&&change_event){
      if(m.object_subtype!=scratchbird::core::catalog::CatalogObjectSubtype::catalog_metadata_change||!c::IsCatalogMetadataChangePayload(m.record.payload)||
          captured.role!=6||row.name_payload||!c::CatalogMetadataChangeMatchesDatabase(m,capture.source()->database_uuid))
        return fail(E::invalid_event,id);
      ++out.events;
    }else if(m.record.header.kind==c::CatalogRecordKind::audit_evidence){
      if(m.object_subtype!=scratchbird::core::catalog::CatalogObjectSubtype::node_construction_intent||!c::IsCatalogConstructionIntentPayload(m.record.payload))
        return fail(E::unsupported_event,id);
      if(captured.role!=6||row.name_payload||!c::CatalogConstructionIntentMatchesDatabase(m,capture.source()->database_uuid))
        return fail(E::invalid_event,id);
      const auto event=c::DecodeCatalogConstructionIntent(m.record.payload);
      const auto member=std::find_if(capture.filespaces().begin(),capture.filespaces().end(),[&](const auto& file){
        return file.bootstrap.filespace_uuid==event.record->filespace_uuid&&file.bootstrap.filespace_role==1&&
            file.bootstrap.database_uuid==event.record->database_uuid;
      });
      if(member==capture.filespaces().end())return fail(E::event_binding,id,event.record->filespace_uuid);
      ++out.events;
    }
    const auto* target=find(m.audit_uuid.value);
    if(!target)return fail(E::missing_event,id,m.audit_uuid.value);
    const auto& evidence=target->row.metadata;
    if(m.audit_uuid.kind!=core::platform::UuidKind::object||target->role!=6||
        evidence.record.header.kind!=c::CatalogRecordKind::audit_evidence)
      return fail(E::invalid_event,id,m.audit_uuid.value);
    const bool name_row=captured.role==2&&row.name_payload;
    if(evidence.object_subtype==scratchbird::core::catalog::CatalogObjectSubtype::catalog_metadata_change&&c::IsCatalogMetadataChangePayload(evidence.record.payload)){
      if(!c::CatalogMetadataChangeMatchesDatabase(evidence,capture.source()->database_uuid))
        return fail(E::invalid_event,id,m.audit_uuid.value);
      const auto event=c::DecodeCatalogMetadataChange(evidence.record.payload);
      if(!name_row&&id!=event.record->event_uuid&&
          (m.creator_transaction_uuid.value!=event.record->origin_transaction_uuid.value||
           m.creator_local_transaction_id!=event.record->origin_local_transaction_id))
        return fail(E::event_binding,id,m.audit_uuid.value);
      ++out.references;
      continue;
    }
    if(evidence.object_subtype!=scratchbird::core::catalog::CatalogObjectSubtype::node_construction_intent||!c::IsCatalogConstructionIntentPayload(evidence.record.payload))
      return fail(E::unsupported_event,id,m.audit_uuid.value);
    if(!c::CatalogConstructionIntentMatchesDatabase(evidence,capture.source()->database_uuid))
      return fail(E::invalid_event,id,m.audit_uuid.value);
    const auto event=c::DecodeCatalogConstructionIntent(evidence.record.payload);
    // The event can be preserved under subsequent admitted metadata versions.
    // Other mutations cannot reuse pre-policy construction intent as their audit.
    if(!name_row&&id!=event.record->event_uuid&&
        (m.creator_transaction_uuid.value!=event.record->origin_transaction_uuid.value||
         m.creator_local_transaction_id!=event.record->origin_local_transaction_id))
      return fail(E::event_binding,id,m.audit_uuid.value);
    ++out.references;
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
