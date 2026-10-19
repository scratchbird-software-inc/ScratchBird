// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
// Naming configuration only. A stored mask is not executable dialect meaning,
// a parser activation, authorization or a substitute for its referenced profile.
struct CatalogNamingDialectProfile {
  Uuid profile_uuid,database_uuid,identifier_profile_uuid;
  u64 generation=0,identifier_profile_generation=0,allowed_dialect_mode_flags=0;
  TypedUuid origin_transaction_uuid,creating_transaction_uuid;
  u64 origin_local_transaction_id=0,creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogNamingDialectProfileSchemaId=65628;
inline constexpr std::size_t kCatalogNamingDialectProfileBytes=224;
struct CatalogNamingDialectProfileResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogNamingDialectProfile> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogNamingDialectProfileEncodedBytes(const CatalogNamingDialectProfile&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogNamingDialectProfileInto(const CatalogNamingDialectProfile&,std::span<byte>) noexcept;
CatalogNamingDialectProfileResult DecodeCatalogNamingDialectProfile(std::string_view) noexcept;
bool IsCatalogNamingDialectProfilePayload(std::string_view) noexcept;
bool CatalogNamingDialectProfileMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogNamingDialectProfileMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogNamingDialectProfileMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogNamingDialectProfilePreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
} // namespace scratchbird::core::catalog
