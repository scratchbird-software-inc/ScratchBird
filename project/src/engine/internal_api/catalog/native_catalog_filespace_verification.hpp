// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_filespace_record_codec.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeCatalogFilespaceError {
  none,source_required,workspace_required,invalid_manifest,database_binding,
  unknown_filespace,duplicate_filespace,physical_binding,missing_manifest
};
struct NativeCatalogFilespaceVerification {
  NativeCatalogFilespaceError error=NativeCatalogFilespaceError::source_required;
  core::platform::Uuid object_uuid,filespace_uuid;
  std::size_t manifests=0;
  bool ok() const noexcept{return error==NativeCatalogFilespaceError::none;}
};
// Exact catalog-to-selected-directory identity, role and lifecycle closure.
// The directory was read under the same source fence as the captured catalog.
// Manifest validation flags remain declarations; this component does not
// perform repair, change lifecycle state, or issue startup/serving authority.
inline NativeCatalogFilespaceVerification VerifyNativeCatalogFilespaces(
    const storage::database::NativeCatalogCapture& capture,const core::platform::Uuid& database_object,
    std::span<core::platform::byte> workspace) noexcept {
  namespace c=core::catalog;using E=NativeCatalogFilespaceError;
  NativeCatalogFilespaceVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid filespace={}){
    out.error=error;out.object_uuid=object;out.filespace_uuid=filespace;out.manifests=0;return out;};
  if(!capture.ok()||database_object.is_nil()||capture.filespaces().empty())return out;
  const auto members=capture.filespaces();
  if(workspace.size()<members.size()||!capture.DisjointWorkspace(std::as_bytes(workspace)))
    return fail(E::workspace_required);
  std::fill(workspace.begin(),workspace.begin()+members.size(),0);
  for(const auto& captured:capture.rows()){
    const auto& m=captured.row.metadata;const auto& h=m.record.header;
    if(h.deleted||h.kind!=c::CatalogRecordKind::filespace)continue;
    const auto decoded=c::DecodeCatalogFilespaceRecord(m.record.payload);
    if(captured.role!=1||captured.row.provisional||!decoded.ok()||m.object_subtype!="filespace"||
        h.parent_uuid.value!=database_object||!m.owning_schema_uuid.value.is_nil())
      return fail(E::invalid_manifest,h.object_uuid.value);
    const auto& manifest=*decoded.record;const auto id=manifest.filespace_uuid.value;
    if(manifest.database_uuid.value!=capture.selection()->header.database_uuid)
      return fail(E::database_binding,h.object_uuid.value,id);
    const auto at=std::lower_bound(members.begin(),members.end(),id,
        [](const auto& member,const auto& value){return member.bootstrap.filespace_uuid<value;});
    if(at==members.end()||at->bootstrap.filespace_uuid!=id)return fail(E::unknown_filespace,h.object_uuid.value,id);
    const auto index=static_cast<std::size_t>(at-members.begin());
    if(workspace[index])return fail(E::duplicate_filespace,h.object_uuid.value,id);
    const bool primary=id==capture.selection()->header.filespace_uuid;
    if(manifest.filespace_role!=at->bootstrap.filespace_role||manifest.state!=at->bootstrap.lifecycle_state||
        manifest.first_filespace!=primary||(primary&&(!manifest.startup_authority||
          !manifest.catalog_persistence_owner||!manifest.filespace_manifest_owner||!manifest.recovery_evidence_owner)))
      return fail(E::physical_binding,h.object_uuid.value,id);
    workspace[index]=1;++out.manifests;
  }
  for(std::size_t i=0;i<members.size();++i)if(!workspace[i])
    return fail(E::missing_manifest,{},members[i].bootstrap.filespace_uuid);
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
