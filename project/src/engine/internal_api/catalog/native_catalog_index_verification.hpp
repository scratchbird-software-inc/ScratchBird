// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_native_catalog_index.hpp"
#include "hash_digest.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeCatalogIndexVerificationError {
  none,source_required,name_relation_identity,missing_dependency_map,invalid_dependency_map,
  dependency_map_binding,dependency_map_digest,missing_key_profile,invalid_key_profile,
  missing_visibility_profile,invalid_visibility_profile,hash_failure
};
struct NativeCatalogIndexVerification {
  NativeCatalogIndexVerificationError error=NativeCatalogIndexVerificationError::source_required;
  core::platform::u16 role=0;
  core::platform::Uuid referenced_uuid;
  std::size_t indexed_relations=0;
  bool ok() const noexcept{return error==NativeCatalogIndexVerificationError::none;}
};
// SEARCH_KEY: SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision
// 2026-11-04, option a). One semantic component of startup verification. The
// storage reader adopts each catalog index's dependency tuple from the index's
// own pages; this resolves the dependency map and both profiles that tuple
// names, in the captured catalog, and requires the map's identities and
// generations to equal the tuple and its encoded bytes to hash to the tuple's
// digest. The names relation must carry the shared constant identity
// (SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID). Heap-free.
inline NativeCatalogIndexVerification VerifyNativeCatalogIndexBindings(
    const storage::database::NativeCatalogCapture& capture) noexcept {
  namespace c=core::catalog;using E=NativeCatalogIndexVerificationError;using core::platform::Uuid;
  NativeCatalogIndexVerification out;
  const auto fail=[&](E error,core::platform::u16 role,const Uuid& reference){
    out.error=error;out.role=role;out.referenced_uuid=reference;out.indexed_relations=0;return out;};
  if(!capture.ok()||!capture.source())return out;
  const auto database=capture.source()->database_uuid;
  const auto rows=capture.rows();
  // A live definition with this identity. Name rows never carry these families.
  const auto find=[&](const Uuid& id)->const storage::database::NativeCatalogVisibleRow*{
    for(const auto& captured:rows){const auto& row=captured.row;
      if(row.name_payload||row.metadata.record.header.deleted||row.metadata.record.header.object_uuid.value!=id)continue;
      return &row;}
    return nullptr;};
  for(const auto& relation:capture.relations()){
    if(relation.role==2&&relation.relation_uuid!=c::kNativeNameCatalogRelationUuid)
      return fail(E::name_relation_identity,relation.role,relation.relation_uuid);
    if(!relation.index_dependencies)continue;
    const auto& d=*relation.index_dependencies;
    const auto* map_row=find(d.dependency_map_uuid);
    if(!map_row)return fail(E::missing_dependency_map,relation.role,d.dependency_map_uuid);
    const auto& m=map_row->metadata;
    const auto map=c::DecodeCatalogNativeCatalogIndexDependencyMap(m.record.payload);
    if(!map.ok()||!c::CatalogNativeCatalogIndexMatchesDatabase(m,database))
      return fail(E::invalid_dependency_map,relation.role,d.dependency_map_uuid);
    const auto& r=*map.record;
    if(r.relation_role!=relation.role||r.relation_uuid!=relation.relation_uuid||r.index_uuid!=d.index_uuid||
        r.key_profile_uuid!=d.key_profile_uuid||r.visibility_profile_uuid!=d.visibility_profile_uuid||
        r.descriptor_generation!=d.descriptor_generation||r.storage_generation!=d.storage_generation)
      return fail(E::dependency_map_binding,relation.role,d.dependency_map_uuid);
    const auto digest=core::hash::ComputeSha256DigestNative(
        reinterpret_cast<const core::platform::byte*>(m.record.payload.data()),m.record.payload.size());
    if(!digest.ok())return fail(E::hash_failure,relation.role,d.dependency_map_uuid);
    if(digest.digest!=d.dependency_map_sha256)return fail(E::dependency_map_digest,relation.role,d.dependency_map_uuid);
    const auto* key_row=find(d.key_profile_uuid);
    if(!key_row)return fail(E::missing_key_profile,relation.role,d.key_profile_uuid);
    if(!c::DecodeCatalogNativeCatalogIndexKeyProfile(key_row->metadata.record.payload).ok()||
        !c::CatalogNativeCatalogIndexMatchesDatabase(key_row->metadata,database))
      return fail(E::invalid_key_profile,relation.role,d.key_profile_uuid);
    const auto* visibility_row=find(d.visibility_profile_uuid);
    if(!visibility_row)return fail(E::missing_visibility_profile,relation.role,d.visibility_profile_uuid);
    if(!c::DecodeCatalogNativeCatalogIndexVisibilityProfile(visibility_row->metadata.record.payload).ok()||
        !c::CatalogNativeCatalogIndexMatchesDatabase(visibility_row->metadata,database))
      return fail(E::invalid_visibility_profile,relation.role,d.visibility_profile_uuid);
    ++out.indexed_relations;
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
