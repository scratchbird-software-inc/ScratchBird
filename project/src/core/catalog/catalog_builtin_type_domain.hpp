// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_object_constants.hpp"
#include "catalog_value_codec.hpp"
#include "catalog_record_codec.hpp"

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_NATIVE_BUILTIN_DATATYPE_DOMAIN_ROWS (content parity,
// 2026-11-05). Create writes one catalog row per built-in datatype and one per
// system domain. The rows are binary and carry no names: a datatype's name and
// a domain's name live only in the name catalog table
// (SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID). Built-in datatypes and system
// domains are fixed values, so their object identities are constants, the same
// in every database: a datatype's is its registered descriptor UUID; a system
// domain's is kSystemDomainUuidPrefix with its datatype's canonical type code
// in the last two bytes.

// 019d0000-0000-7000-8000-00000d00XXXX, XXXX = the canonical type code.
Uuid SystemDomainUuid(u32 canonical_type_code) noexcept;

enum class BuiltinDatatypeFlag : u64 {
  nullable_allowed=1, descriptor_authoritative=2, reference_name_is_alias_only=4, requires_mandatory_library=8
};
inline constexpr u64 kBuiltinDatatypeFlagMask=15;
enum class SystemDomainKind : u64 { system_domain=1 };
enum class SystemDomainFlag : u64 { descriptor_authoritative=1 };
inline constexpr u64 kSystemDomainFlagMask=1;

struct CatalogBuiltinDatatypeDescriptor {
  Uuid descriptor_uuid,database_uuid;
  u64 generation=0;
  // The datatype catalog manifest's epoch the descriptor was taken from.
  u64 catalog_epoch=0;
  // core::datatypes codes, as numbers: CanonicalTypeId, TypeFamily, TypeWidthClass.
  u64 type_code=0,family_code=0,width_class_code=0;
  u64 bit_width=0,default_precision=0,default_scale=0;
  u64 flags=0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
struct CatalogSystemDomain {
  Uuid domain_uuid,database_uuid;
  u64 generation=0;
  SystemDomainKind kind=SystemDomainKind(0);
  Uuid base_descriptor_uuid;
  u64 base_descriptor_generation=0;
  u64 flags=0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogBuiltinDatatypeDescriptorSchemaId=65632;
inline constexpr u32 kCatalogSystemDomainSchemaId=65633;
inline constexpr CatalogObjectSubtype kBuiltinDatatypeDescriptorSubtype=CatalogObjectSubtype::builtin_datatype_descriptor;
inline constexpr CatalogObjectSubtype kSystemDomainSubtype=CatalogObjectSubtype::system_domain;
template<class T> struct CatalogBuiltinTypeDomainResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<T> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogBuiltinDatatypeDescriptorEncodedBytes(const CatalogBuiltinDatatypeDescriptor&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogBuiltinDatatypeDescriptorInto(const CatalogBuiltinDatatypeDescriptor&,std::span<byte>) noexcept;
CatalogValueEncodeResult EncodeCatalogBuiltinDatatypeDescriptor(const CatalogBuiltinDatatypeDescriptor&);
CatalogBuiltinTypeDomainResult<CatalogBuiltinDatatypeDescriptor> DecodeCatalogBuiltinDatatypeDescriptor(std::string_view) noexcept;
std::optional<std::size_t> CatalogSystemDomainEncodedBytes(const CatalogSystemDomain&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogSystemDomainInto(const CatalogSystemDomain&,std::span<byte>) noexcept;
CatalogValueEncodeResult EncodeCatalogSystemDomain(const CatalogSystemDomain&);
CatalogBuiltinTypeDomainResult<CatalogSystemDomain> DecodeCatalogSystemDomain(std::string_view) noexcept;
// Either family, recognised by schema identity.
bool IsCatalogBuiltinTypeDomainPayload(std::string_view) noexcept;
// Family dispatch for the common record codec and the storage admission sites.
// A record outside the two families passes; one inside must decode and bind.
bool CatalogBuiltinTypeDomainMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogBuiltinTypeDomainMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogBuiltinTypeDomainMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogBuiltinTypeDomainPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
// Every identity a record of these families references, for the native
// issuer's exclusion set.
template<class F> void ForEachCatalogBuiltinTypeDomainReference(std::string_view payload,F&& reserve) {
  if(const auto d=DecodeCatalogBuiltinDatatypeDescriptor(payload);d.ok()) {
    const auto& r=*d.record;
    for(const auto& id:{r.descriptor_uuid,r.database_uuid,r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})reserve(id);
  } else if(const auto s=DecodeCatalogSystemDomain(payload);s.ok()) {
    const auto& r=*s.record;
    for(const auto& id:{r.domain_uuid,r.database_uuid,r.base_descriptor_uuid,r.origin_transaction_uuid.value,
        r.creating_transaction_uuid.value})reserve(id);
  }
}
} // namespace scratchbird::core::catalog
