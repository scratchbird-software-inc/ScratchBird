// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_catalog_index_rows.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E=NativeCatalogIndexRowsError;
namespace c=core::catalog;
using core::platform::Uuid;
NativeCatalogIndexRowsResult Fail(E error){NativeCatalogIndexRowsResult out;out.error=error;return out;}
std::string Payload(const std::vector<core::platform::byte>& bytes){return {bytes.begin(),bytes.end()};}
} // namespace

NativeCatalogIndexRowsResult BuildNativeCatalogIndexRows(const NativeCatalogIndexRowsRequest& request) noexcept {
  try {
    if(!core::uuid::IsEngineIdentityUuid(request.database_uuid)||
        request.construction_transaction.kind!=core::platform::UuidKind::transaction||
        !core::uuid::IsEngineIdentityUuid(request.construction_transaction.value)||
        !request.construction_local_transaction_id)return Fail(E::invalid_request);
    // Every identity a binding carries is distinct across the whole request,
    // including the constants and the database and transaction identities.
    std::vector<Uuid> taken{request.database_uuid,request.construction_transaction.value,
        c::kNativeNameCatalogRelationUuid,c::kNativeCatalogIndexKeyProfileUuid,
        c::kNativeCatalogIndexVisibilityProfileUuid,c::kNativeBtreeIndexFamilyUuid,
        c::kNativeNameAuthoritativeTreeKeyProfileUuid,c::kNativeNameUuidTreeKeyProfileUuid};
    std::array<Uuid,6> relations{};
    bool any=false;
    for(unsigned i=0;i<6;++i){
      const auto& role=request.roles[i];if(!role)continue;any=true;
      if(i==1?!role->relation_uuid.is_nil():!core::uuid::IsEngineIdentityUuid(role->relation_uuid))return Fail(E::invalid_request);
      relations[i]=i==1?c::kNativeNameCatalogRelationUuid:role->relation_uuid;
      if(!core::uuid::IsEngineIdentityUuid(role->index_uuid)||!core::uuid::IsEngineIdentityUuid(role->map_uuid))
        return Fail(E::invalid_request);
      if(i!=1)taken.push_back(relations[i]);
      taken.push_back(role->index_uuid);taken.push_back(role->map_uuid);
    }
    if(request.name_trees){
      const auto& t=*request.name_trees;
      for(const auto& id:{t.authoritative_index_uuid,t.authoritative_map_uuid,t.uuid_index_uuid,t.uuid_map_uuid}){
        if(!core::uuid::IsEngineIdentityUuid(id))return Fail(E::invalid_request);
        taken.push_back(id);
      }
      any=true;
    }
    std::sort(taken.begin(),taken.end());
    if(std::adjacent_find(taken.begin(),taken.end())!=taken.end())return Fail(E::duplicate_identity);
    NativeCatalogIndexRowsResult out;
    if(!any){out.error=E::none;return out;}
    const auto tx=request.construction_transaction;const auto local=request.construction_local_transaction_id;
    c::CatalogNativeCatalogIndexKeyProfile key;
    key.profile_uuid=c::kNativeCatalogIndexKeyProfileUuid;key.database_uuid=request.database_uuid;key.generation=1;
    key.key_form=c::NativeCatalogIndexKeyForm::raw16_canonical_row_uuid;
    key.key_order=c::NativeCatalogIndexKeyOrder::unsigned_canonical_bytes;
    key.tie_break=c::NativeCatalogIndexTieBreak::native_row_then_version_uuid;
    key.origin_transaction_uuid=key.creating_transaction_uuid=tx;
    key.origin_local_transaction_id=key.creating_local_transaction_id=local;
    c::CatalogNativeCatalogIndexVisibilityProfile visibility;
    visibility.profile_uuid=c::kNativeCatalogIndexVisibilityProfileUuid;visibility.database_uuid=request.database_uuid;
    visibility.generation=1;visibility.visibility=c::NativeCatalogIndexVisibility::native_mga_inventory_creator;
    visibility.origin_transaction_uuid=visibility.creating_transaction_uuid=tx;
    visibility.origin_local_transaction_id=visibility.creating_local_transaction_id=local;
    const auto encoded_key=c::EncodeCatalogNativeCatalogIndexKeyProfile(key);
    const auto encoded_visibility=c::EncodeCatalogNativeCatalogIndexVisibilityProfile(visibility);
    if(!encoded_key.ok()||!encoded_visibility.ok())return Fail(E::encoding_failure);
    out.rows.reserve(8);
    out.rows.push_back({key.profile_uuid,c::CatalogObjectSubtype::native_catalog_index_key_profile,Payload(encoded_key.bytes)});
    out.rows.push_back({visibility.profile_uuid,c::CatalogObjectSubtype::native_catalog_index_visibility_profile,Payload(encoded_visibility.bytes)});
    for(unsigned i=0;i<6;++i){
      const auto& role=request.roles[i];if(!role)continue;
      c::CatalogNativeCatalogIndexDependencyMap map;
      map.map_uuid=role->map_uuid;map.database_uuid=request.database_uuid;map.generation=1;map.relation_role=i+1;
      map.relation_uuid=relations[i];map.index_uuid=role->index_uuid;map.index_family_uuid=c::kNativeBtreeIndexFamilyUuid;
      map.key_profile_uuid=key.profile_uuid;map.visibility_profile_uuid=visibility.profile_uuid;
      map.descriptor_generation=map.storage_generation=1;
      map.origin_transaction_uuid=map.creating_transaction_uuid=tx;
      map.origin_local_transaction_id=map.creating_local_transaction_id=local;
      const auto encoded=c::EncodeCatalogNativeCatalogIndexDependencyMap(map);
      if(!encoded.ok())return Fail(E::encoding_failure);
      const auto digest=core::hash::ComputeSha256Digest(encoded.bytes);
      if(!digest.ok())return Fail(E::hash_failure);
      page::NativeBtreeDependencies dependencies;
      dependencies.index_uuid=map.index_uuid;
      dependencies.descriptor_generation=map.descriptor_generation;dependencies.storage_generation=map.storage_generation;
      dependencies.key_profile_uuid=map.key_profile_uuid;dependencies.visibility_profile_uuid=map.visibility_profile_uuid;
      dependencies.dependency_map_uuid=map.map_uuid;dependencies.dependency_map_sha256=digest.digest;
      out.indexes[i]=NativeCreationCatalogIndexBinding{map.relation_uuid,dependencies};
      out.rows.push_back({map.map_uuid,c::CatalogObjectSubtype::native_catalog_index_dependency_map,Payload(encoded.bytes)});
    }
    if(request.name_trees){
      // SB_NATIVE_NAME_TREES: the two name trees over the names relation (role
      // 2): a fixed key profile each, and an issued index and map each.
      const auto& t=*request.name_trees;
      struct Tree{Uuid profile;c::NativeCatalogIndexKeyForm form;Uuid index,map;std::optional<page::NativeBtreeDependencies>* out;};
      const Tree trees[2]{
        {c::kNativeNameAuthoritativeTreeKeyProfileUuid,c::NativeCatalogIndexKeyForm::name_authoritative_v1,
         t.authoritative_index_uuid,t.authoritative_map_uuid,&out.name_trees.authoritative},
        {c::kNativeNameUuidTreeKeyProfileUuid,c::NativeCatalogIndexKeyForm::name_uuid_v1,
         t.uuid_index_uuid,t.uuid_map_uuid,&out.name_trees.uuid}};
      for(const auto& tree:trees){
        auto profile=key;profile.profile_uuid=tree.profile;profile.key_form=tree.form;
        const auto encoded_profile=c::EncodeCatalogNativeCatalogIndexKeyProfile(profile);
        if(!encoded_profile.ok())return Fail(E::encoding_failure);
        out.rows.push_back({profile.profile_uuid,c::CatalogObjectSubtype::native_catalog_index_key_profile,Payload(encoded_profile.bytes)});
      }
      for(const auto& tree:trees){
        c::CatalogNativeCatalogIndexDependencyMap map;
        map.map_uuid=tree.map;map.database_uuid=request.database_uuid;map.generation=1;map.relation_role=2;
        map.relation_uuid=c::kNativeNameCatalogRelationUuid;map.index_uuid=tree.index;map.index_family_uuid=c::kNativeBtreeIndexFamilyUuid;
        map.key_profile_uuid=tree.profile;map.visibility_profile_uuid=visibility.profile_uuid;
        map.descriptor_generation=map.storage_generation=1;
        map.origin_transaction_uuid=map.creating_transaction_uuid=tx;
        map.origin_local_transaction_id=map.creating_local_transaction_id=local;
        const auto encoded=c::EncodeCatalogNativeCatalogIndexDependencyMap(map);
        if(!encoded.ok())return Fail(E::encoding_failure);
        const auto digest=core::hash::ComputeSha256Digest(encoded.bytes);
        if(!digest.ok())return Fail(E::hash_failure);
        page::NativeBtreeDependencies dependencies;
        dependencies.index_uuid=map.index_uuid;
        dependencies.descriptor_generation=map.descriptor_generation;dependencies.storage_generation=map.storage_generation;
        dependencies.key_profile_uuid=map.key_profile_uuid;dependencies.visibility_profile_uuid=map.visibility_profile_uuid;
        dependencies.dependency_map_uuid=map.map_uuid;dependencies.dependency_map_sha256=digest.digest;
        *tree.out=dependencies;
        out.rows.push_back({map.map_uuid,c::CatalogObjectSubtype::native_catalog_index_dependency_map,Payload(encoded.bytes)});
      }
    }
    out.error=E::none;return out;
  } catch(const std::bad_alloc&) {return Fail(E::resource_exhausted);}
    catch(const std::length_error&) {return Fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database
