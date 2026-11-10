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
  // SB_NATIVE_NAME_TREES: the name trees whose tuples resolved (0 or 2).
  std::size_t indexed_name_trees=0;
  bool ok() const noexcept{return error==NativeCatalogIndexVerificationError::none;}
};
// SEARCH_KEY: SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision
// 2026-11-04, option a). One semantic component of startup verification. The
// storage reader adopts each catalog index's dependency tuple from the index's
// own pages; this resolves the dependency map and both profiles that tuple
// names, in the captured catalog, and requires the map's identities and
// generations to equal the tuple and its encoded bytes to hash to the tuple's
// digest. The names relation must carry the shared constant identity
// (SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID). SB_NATIVE_NAME_TREES: the two
// name trees' tuples resolve the same way, as role 2 over the names relation,
// each to the fixed key profile of its form. Heap-free.
inline NativeCatalogIndexVerification VerifyNativeCatalogIndexBindings(
    const storage::database::NativeCatalogCapture& capture) noexcept {
  namespace c=core::catalog;using E=NativeCatalogIndexVerificationError;using core::platform::Uuid;
  NativeCatalogIndexVerification out;
  const auto fail=[&](E error,core::platform::u16 role,const Uuid& reference){
    out.error=error;out.role=role;out.referenced_uuid=reference;out.indexed_relations=0;out.indexed_name_trees=0;return out;};
  if(!capture.ok()||!capture.source())return out;
  const auto database=capture.source()->database_uuid;
  const auto rows=capture.rows();
  // A live definition with this identity. Name rows never carry these families.
  const auto find=[&](const Uuid& id)->const storage::database::NativeCatalogVisibleRow*{
    for(const auto& captured:rows){const auto& row=captured.row;
      if(row.name_payload||row.metadata.record.header.deleted||row.metadata.record.header.object_uuid.value!=id)continue;
      return &row;}
    return nullptr;};
  // One tuple: its map row names this relation, index and profiles with the
  // tuple's generations and hashes to the tuple's digest; both profile rows
  // decode; the key profile's form is `form` when one is required. Returns the
  // error, or none.
  const auto resolve=[&](core::platform::u16 role,const Uuid& relation_uuid,const storage::page::NativeBtreeDependencies& d,
      std::optional<c::NativeCatalogIndexKeyForm> form,Uuid& reference)->E{
    const auto* map_row=find(d.dependency_map_uuid);
    reference=d.dependency_map_uuid;
    if(!map_row)return E::missing_dependency_map;
    const auto& m=map_row->metadata;
    const auto map=c::DecodeCatalogNativeCatalogIndexDependencyMap(m.record.payload);
    if(!map.ok()||!c::CatalogNativeCatalogIndexMatchesDatabase(m,database))return E::invalid_dependency_map;
    const auto& r=*map.record;
    if(r.relation_role!=role||r.relation_uuid!=relation_uuid||r.index_uuid!=d.index_uuid||
        r.key_profile_uuid!=d.key_profile_uuid||r.visibility_profile_uuid!=d.visibility_profile_uuid||
        r.descriptor_generation!=d.descriptor_generation||r.storage_generation!=d.storage_generation)
      return E::dependency_map_binding;
    const auto digest=core::hash::ComputeSha256DigestNative(
        reinterpret_cast<const core::platform::byte*>(m.record.payload.data()),m.record.payload.size());
    if(!digest.ok())return E::hash_failure;
    if(digest.digest!=d.dependency_map_sha256)return E::dependency_map_digest;
    reference=d.key_profile_uuid;
    const auto* key_row=find(d.key_profile_uuid);
    if(!key_row)return E::missing_key_profile;
    const auto key=c::DecodeCatalogNativeCatalogIndexKeyProfile(key_row->metadata.record.payload);
    if(!key.ok()||!c::CatalogNativeCatalogIndexMatchesDatabase(key_row->metadata,database)||
        (form&&key.record->key_form!=*form))return E::invalid_key_profile;
    reference=d.visibility_profile_uuid;
    const auto* visibility_row=find(d.visibility_profile_uuid);
    if(!visibility_row)return E::missing_visibility_profile;
    if(!c::DecodeCatalogNativeCatalogIndexVisibilityProfile(visibility_row->metadata.record.payload).ok()||
        !c::CatalogNativeCatalogIndexMatchesDatabase(visibility_row->metadata,database))
      return E::invalid_visibility_profile;
    return E::none;
  };
  for(const auto& relation:capture.relations()){
    if(relation.role==2&&relation.relation_uuid!=c::kNativeNameCatalogRelationUuid)
      return fail(E::name_relation_identity,relation.role,relation.relation_uuid);
    if(!relation.index_dependencies)continue;
    Uuid reference;
    if(const auto error=resolve(relation.role,relation.relation_uuid,*relation.index_dependencies,std::nullopt,reference);error!=E::none)
      return fail(error,relation.role,reference);
    ++out.indexed_relations;
  }
  const auto& trees=capture.name_trees();
  for(unsigned t=0;t<2;++t){
    if(!trees[t])continue;
    const auto& d=trees[t]->dependencies;
    const auto profile=t?c::kNativeNameUuidTreeKeyProfileUuid:c::kNativeNameAuthoritativeTreeKeyProfileUuid;
    if(d.key_profile_uuid!=profile)return fail(E::invalid_key_profile,2,d.key_profile_uuid);
    Uuid reference;
    if(const auto error=resolve(2,c::kNativeNameCatalogRelationUuid,d,
        t?c::NativeCatalogIndexKeyForm::name_uuid_v1:c::NativeCatalogIndexKeyForm::name_authoritative_v1,reference);error!=E::none)
      return fail(error,2,reference);
    ++out.indexed_name_trees;
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
