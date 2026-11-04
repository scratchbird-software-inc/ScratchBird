// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include "catalog_record_codec.hpp"

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision
// 2026-11-04, option a). A native catalog relation's B-tree index binding
// references a key profile, a visibility profile and a dependency map. These
// are the catalog rows behind those references: create writes them in the same
// construction, and opening the file resolves them and compares each stored
// index tuple against them. The map's digest is SHA-256 over its encoded bytes.
//
// SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID (owner direction 2026-11-04):
// fixed values are constant UUIDs, the same in every database. The name
// relation is the one catalog relation with a constant identity; the two
// catalog index profiles are fixed values too. Dependency maps are per relation
// and per database, so their identities are issued.
inline constexpr Uuid kNativeNameCatalogRelationUuid{
    {0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0x0c,0xa7,0x01}};
inline constexpr Uuid kNativeCatalogIndexKeyProfileUuid{
    {0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0x0c,0xa7,0x02}};
inline constexpr Uuid kNativeCatalogIndexVisibilityProfileUuid{
    {0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0x0c,0xa7,0x03}};
// The btree global index family (registries/native-index-family-identity-registry.csv).
inline constexpr Uuid kNativeBtreeIndexFamilyUuid{
    {0x96,0xa7,0xb8,0xc9,0xda,0xeb,0x7c,0x0d,0x9e,0x2f,0x40,0x51,0x62,0x73,0x84,0x95}};

// Closed vocabularies. One value each today: the catalog relation index keys
// are the raw16 canonical row UUID in unsigned canonical-byte order, tie-broken
// by the native row then version identity, and every entry is an access hint
// whose row is resolved against the selected transaction inventory.
enum class NativeCatalogIndexKeyForm : u64 { raw16_canonical_row_uuid=1 };
enum class NativeCatalogIndexKeyOrder : u64 { unsigned_canonical_bytes=1 };
enum class NativeCatalogIndexTieBreak : u64 { native_row_then_version_uuid=1 };
enum class NativeCatalogIndexVisibility : u64 { native_mga_inventory_creator=1 };

struct CatalogNativeCatalogIndexKeyProfile {
  Uuid profile_uuid,database_uuid;
  u64 generation=0;
  NativeCatalogIndexKeyForm key_form=NativeCatalogIndexKeyForm(0);
  NativeCatalogIndexKeyOrder key_order=NativeCatalogIndexKeyOrder(0);
  NativeCatalogIndexTieBreak tie_break=NativeCatalogIndexTieBreak(0);
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
struct CatalogNativeCatalogIndexVisibilityProfile {
  Uuid profile_uuid,database_uuid;
  u64 generation=0;
  NativeCatalogIndexVisibility visibility=NativeCatalogIndexVisibility(0);
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
// Identities and generations only: no page reference or row count, so create
// can compute the digest before building page images and a republication of
// the same index leaves it unchanged.
struct CatalogNativeCatalogIndexDependencyMap {
  Uuid map_uuid,database_uuid;
  u64 generation=0;
  // The catalog relation role, 1..6.
  u64 relation_role=0;
  Uuid relation_uuid,index_uuid,index_family_uuid,key_profile_uuid,visibility_profile_uuid;
  u64 descriptor_generation=0,storage_generation=0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogNativeCatalogIndexKeyProfileSchemaId=65629;
inline constexpr u32 kCatalogNativeCatalogIndexVisibilityProfileSchemaId=65630;
inline constexpr u32 kCatalogNativeCatalogIndexDependencyMapSchemaId=65631;
inline constexpr std::size_t kCatalogNativeCatalogIndexKeyProfileBytes=216;
inline constexpr std::size_t kCatalogNativeCatalogIndexVisibilityProfileBytes=184;
inline constexpr std::size_t kCatalogNativeCatalogIndexDependencyMapBytes=336;
template<class T> struct CatalogNativeCatalogIndexResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<T> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
// Heap-free size/Into encode and decode; the owning encode allocates its result.
std::optional<std::size_t> CatalogNativeCatalogIndexKeyProfileEncodedBytes(const CatalogNativeCatalogIndexKeyProfile&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexKeyProfileInto(const CatalogNativeCatalogIndexKeyProfile&,std::span<byte>) noexcept;
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexKeyProfile(const CatalogNativeCatalogIndexKeyProfile&);
CatalogNativeCatalogIndexResult<CatalogNativeCatalogIndexKeyProfile> DecodeCatalogNativeCatalogIndexKeyProfile(std::string_view) noexcept;
std::optional<std::size_t> CatalogNativeCatalogIndexVisibilityProfileEncodedBytes(const CatalogNativeCatalogIndexVisibilityProfile&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexVisibilityProfileInto(const CatalogNativeCatalogIndexVisibilityProfile&,std::span<byte>) noexcept;
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexVisibilityProfile(const CatalogNativeCatalogIndexVisibilityProfile&);
CatalogNativeCatalogIndexResult<CatalogNativeCatalogIndexVisibilityProfile> DecodeCatalogNativeCatalogIndexVisibilityProfile(std::string_view) noexcept;
std::optional<std::size_t> CatalogNativeCatalogIndexDependencyMapEncodedBytes(const CatalogNativeCatalogIndexDependencyMap&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogNativeCatalogIndexDependencyMapInto(const CatalogNativeCatalogIndexDependencyMap&,std::span<byte>) noexcept;
CatalogValueEncodeResult EncodeCatalogNativeCatalogIndexDependencyMap(const CatalogNativeCatalogIndexDependencyMap&);
CatalogNativeCatalogIndexResult<CatalogNativeCatalogIndexDependencyMap> DecodeCatalogNativeCatalogIndexDependencyMap(std::string_view) noexcept;
// Any of the three families, recognised by schema identity.
bool IsCatalogNativeCatalogIndexPayload(std::string_view) noexcept;
// Family dispatch for the common record codec and the storage admission sites.
// A record outside the three families passes; one inside must decode and bind.
bool CatalogNativeCatalogIndexMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogNativeCatalogIndexMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogNativeCatalogIndexMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogNativeCatalogIndexPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
// Every identity a record of these families references, for the native
// issuer's exclusion set.
template<class F> void ForEachCatalogNativeCatalogIndexReference(std::string_view payload,F&& reserve);
} // namespace scratchbird::core::catalog

namespace scratchbird::core::catalog {
template<class F> void ForEachCatalogNativeCatalogIndexReference(std::string_view payload,F&& reserve) {
  if(const auto k=DecodeCatalogNativeCatalogIndexKeyProfile(payload);k.ok()) {
    const auto& r=*k.record;
    for(const auto& id:{r.profile_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})reserve(id);
  } else if(const auto v=DecodeCatalogNativeCatalogIndexVisibilityProfile(payload);v.ok()) {
    const auto& r=*v.record;
    for(const auto& id:{r.profile_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})reserve(id);
  } else if(const auto m=DecodeCatalogNativeCatalogIndexDependencyMap(payload);m.ok()) {
    const auto& r=*m.record;
    for(const auto& id:{r.map_uuid,r.database_uuid,r.relation_uuid,r.index_uuid,r.index_family_uuid,r.key_profile_uuid,
        r.visibility_profile_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})reserve(id);
  }
}
} // namespace scratchbird::core::catalog
