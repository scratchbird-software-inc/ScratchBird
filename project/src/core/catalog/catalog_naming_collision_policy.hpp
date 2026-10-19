// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <array>

namespace scratchbird::core::catalog {
enum class CatalogCollisionLanguageLane:u8 { exact_tag=1,ascii_folded_tag,all_languages };
enum class CatalogCollisionKeyLane:u8 { separate=1,shared };
struct CatalogCollisionClassBinding {CatalogRecordKind kind=CatalogRecordKind::unknown;Uuid group_uuid;};
struct CatalogCollisionGroup {
  Uuid group_uuid;
  CatalogCollisionLanguageLane language_lane=static_cast<CatalogCollisionLanguageLane>(0);
  CatalogCollisionKeyLane key_lane=static_cast<CatalogCollisionKeyLane>(0);
};
inline constexpr std::size_t kCatalogNamingCollisionMaximumBindings=64;
inline constexpr std::size_t kCatalogNamingCollisionPolicyMaximumBytes=2872;
inline constexpr u32 kCatalogNamingCollisionPolicySchemaId=65627;
// Explicit supplied rules; there is no inferred namespace, language or quote
// default. Embedded group identities are owned by this body's MGA generation.
struct CatalogNamingCollisionPolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  std::array<CatalogCollisionClassBinding,kCatalogNamingCollisionMaximumBindings> classes{};
  std::array<CatalogCollisionGroup,kCatalogNamingCollisionMaximumBindings> groups{};
  std::size_t class_count=0,group_count=0;
  TypedUuid origin_transaction_uuid,creating_transaction_uuid;
  u64 origin_local_transaction_id=0,creating_local_transaction_id=0;
};
struct CatalogNamingCollisionPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogNamingCollisionPolicyBody> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogNamingCollisionPolicyEncodedBytes(const CatalogNamingCollisionPolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogNamingCollisionPolicyInto(const CatalogNamingCollisionPolicyBody&,std::span<byte>) noexcept;
CatalogNamingCollisionPolicyResult DecodeCatalogNamingCollisionPolicy(std::string_view) noexcept;
bool IsCatalogNamingCollisionPolicyPayload(std::string_view) noexcept;
bool CatalogNamingCollisionPolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogNamingCollisionPolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogNamingCollisionPolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogNamingCollisionPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
} // namespace scratchbird::core::catalog
