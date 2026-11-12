// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_name_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <algorithm>
#include <array>
#include <string_view>

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
// SB_NAME_COLLISION_ORDER: the one collision order over active name entries,
// shared by startup collision verification and the native catalog writer. Two
// entries of different objects collide when their scope order is 0, they bind
// the same group of the same policy body, and their lane order under that
// group is 0. Scope order: scope, then identifier profile.
inline int CompareCatalogNameCollisionScope(const CatalogNameEntryView& x,const CatalogNameEntryView& y) noexcept {
  if(x.scope_uuid.value!=y.scope_uuid.value)return x.scope_uuid.value<y.scope_uuid.value?-1:1;
  if(x.identifier_profile_uuid.value!=y.identifier_profile_uuid.value)
    return x.identifier_profile_uuid.value<y.identifier_profile_uuid.value?-1:1;
  return 0;
}
// Lane order under one group: the language (or not at all), then the
// exact-match flag unless the group shares one key lane, then the key of each
// entry's own lane (exact or normalized).
// SB_CATALOG_LANGUAGE_CONSTANTS (owner Q53 = 1, 2026-11-11): a language is a
// constant UUID, so exact_tag compares the UUIDs. ascii_folded_tag compared
// tags with ASCII case folded; tags are folded once where a tag becomes a
// language (CatalogLanguageForTag), so it now compares the UUIDs too - its
// distinction from exact_tag is gone (annotated as drift, value kept).
inline int CompareCatalogNameCollisionLanes(const CatalogNameEntryView& x,const CatalogNameEntryView& y,
    CatalogCollisionLanguageLane language,CatalogCollisionKeyLane key) noexcept {
  if(language!=CatalogCollisionLanguageLane::all_languages){
    const auto& a=x.language_uuid.value.bytes;const auto& b=y.language_uuid.value.bytes;
    if(a!=b)return a<b?-1:1;
  }
  if(key!=CatalogCollisionKeyLane::shared&&x.requires_exact_match!=y.requires_exact_match)return x.requires_exact_match?1:-1;
  const auto xkey=x.requires_exact_match?x.exact_lookup_key:x.normalized_lookup_key;
  const auto ykey=y.requires_exact_match?y.exact_lookup_key:y.normalized_lookup_key;
  if(std::lexicographical_compare(xkey.begin(),xkey.end(),ykey.begin(),ykey.end()))return -1;
  if(std::lexicographical_compare(ykey.begin(),ykey.end(),xkey.begin(),xkey.end()))return 1;
  return 0;
}
} // namespace scratchbird::core::catalog
