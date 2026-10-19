// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_management_descriptor.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeStartupManagementDefinitionError {
  none,source_required,workspace_required,duplicate_identity,missing_descriptor,
  duplicate_descriptor,invalid_descriptor,missing_profile,invalid_profile,profile_code_mismatch
};
struct NativeStartupManagementDefinition {
  NativeStartupManagementDefinitionError error=NativeStartupManagementDefinitionError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  core::catalog::CatalogManagementOperationDescriptor descriptor;
  core::platform::Uuid descriptor_version_uuid;
  std::array<core::catalog::CatalogManagementProfile,10> profiles;
  std::array<core::platform::Uuid,10> profile_versions;
  std::optional<storage::database::NativeCheckpointSelection> selection;
  bool ok()const noexcept{return error==NativeStartupManagementDefinitionError::none&&selection.has_value();}
};
// Resolves actual source-backed declarations, not an invocation capability.
// Implementation registration, parameter validation, maintenance authorization,
// policy admission, resources, evidence and transaction ownership remain engine
// obligations. A caller-supplied descriptor cannot replace this native capture.
inline NativeStartupManagementDefinition ResolveNativeStartupManagementDefinition(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeStartupManagementDefinitionError;using core::platform::Uuid;
  const auto fail=[](E error,Uuid object={},Uuid reference={}){
    NativeStartupManagementDefinition r;r.error=error;r.object_uuid=object;r.referenced_uuid=reference;return r;};
  if(!capture.ok())return fail(E::source_required);
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      reinterpret_cast<std::uintptr_t>(scratch.data())%alignof(std::size_t)||
      !capture.DisjointWorkspace(std::as_bytes(scratch)))return fail(E::workspace_required);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t n){return rows[n].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t n=1;n<order.size();++n)if(identity(order[n-1])==identity(order[n]))return fail(E::duplicate_identity,identity(order[n]));
  const auto find=[&](const Uuid& id)->const storage::database::NativeCapturedCatalogRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto n,const auto& value){return identity(n)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?nullptr:&rows[*at];
  };
  const auto database=capture.selection()->header.database_uuid;
  const auto active=[&](const storage::database::NativeCapturedCatalogRow& r){const auto& m=r.row.metadata;
    return r.role==6&&!r.row.provisional&&!r.row.name_payload&&core::uuid::IsEngineIdentityUuid(r.row.version_uuid)&&
      !m.record.header.deleted&&m.lifecycle==c::CatalogObjectLifecycle::active&&m.status==c::CatalogObjectStatus::active&&
      c::CatalogManagementDefinitionMatchesDatabase(m,database);
  };
  NativeStartupManagementDefinition out;
  for(const auto& r:rows){const auto& m=r.row.metadata;
    if(m.record.header.deleted||(!c::IsCatalogManagementDescriptorPayload(m.record.payload)&&m.object_subtype!="management_descriptor"))continue;
    const auto value=c::DecodeCatalogManagementDescriptor(m.record.payload);
    if(!value.ok()||!active(r))return fail(E::invalid_descriptor,m.record.header.object_uuid.value);
    if(!out.descriptor_version_uuid.is_nil())return fail(E::duplicate_descriptor,m.record.header.object_uuid.value);
    out.descriptor=*value.record;out.descriptor_version_uuid=r.row.version_uuid;
  }
  if(out.descriptor_version_uuid.is_nil())return fail(E::missing_descriptor);
  for(std::size_t i=0;i<out.profiles.size();++i){
    const auto id=out.descriptor.profiles[i];const auto* row=find(id);
    if(!row)return fail(E::missing_profile,out.descriptor.descriptor_uuid,id);
    if(!active(*row)||!c::CatalogManagementProfileMatchesMetadata(row->row.metadata))
      return fail(E::invalid_profile,out.descriptor.descriptor_uuid,id);
    const auto value=c::DecodeCatalogManagementProfile(row->row.metadata.record.payload);
    if(!value.ok())return fail(E::invalid_profile,out.descriptor.descriptor_uuid,id);
    if(core::platform::u64(value.record->code)!=i+1)return fail(E::profile_code_mismatch,out.descriptor.descriptor_uuid,id);
    out.profiles[i]=*value.record;out.profile_versions[i]=row->row.version_uuid;
  }
  out.selection=capture.selection();out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
