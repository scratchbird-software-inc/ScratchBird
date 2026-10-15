// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Native policy values, not names selecting executable behavior. The owning
// resolver must admit the referenced resources and implement the selected
// operation; this carrier neither folds text nor grants parser authority.
enum class IdentifierEscapeRule : u64 { doubled_delimiter=1,backslash,doubled_or_backslash,not_applicable };
enum class IdentifierFoldMode : u64 { none=0,upper_ascii,lower_ascii,resource_upper,resource_lower };
enum class IdentifierComparisonMode : u64 { exact=1,folded,case_insensitive,profile_platform };
enum class IdentifierDisplayMode : u64 { preserve_user_text=1,store_folded_display,donor_projection };
enum class IdentifierReservedWordMode : u64 { reserved_requires_quote=1,contextual,parser_profile_specific };
enum class IdentifierLengthUnit : u64 { unicode_scalar_values=1,encoded_octets };
struct CatalogIdentifierResourceBinding {
  Uuid charset_uuid,collation_uuid;
  u64 charset_generation=0,collation_generation=0;
};
struct CatalogIdentifierPlatformBinding {Uuid profile_uuid;u64 generation=0;};
struct CatalogIdentifierResolutionProfile {
  Uuid profile_uuid,database_uuid;
  u64 generation=0;
  // Display/import/export data only; never used to select a fold algorithm.
  std::string_view profile_name;
  // bits0..3: double_quote,backtick,bracket,donor_api. Zero admits no quotes.
  u64 quote_styles=0;
  IdentifierEscapeRule escape_rule=static_cast<IdentifierEscapeRule>(0);
  IdentifierFoldMode unquoted_fold_mode=IdentifierFoldMode::none;
  bool quoted_preserves_case=false;
  IdentifierComparisonMode comparison_mode=static_cast<IdentifierComparisonMode>(0);
  IdentifierDisplayMode storage_display_mode=static_cast<IdentifierDisplayMode>(0);
  IdentifierReservedWordMode reserved_word_behavior=static_cast<IdentifierReservedWordMode>(0);
  u64 maximum_identifier_length=0;
  IdentifierLengthUnit length_unit=static_cast<IdentifierLengthUnit>(0);
  // Exact dialect-owned data; semantic admission is the selected dialect's
  // closed flag vocabulary, not a generic codec inference or host OS default.
  u64 dialect_mode_flags=0;
  std::optional<CatalogIdentifierResourceBinding> resources;
  std::optional<CatalogIdentifierPlatformBinding> platform;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
};
struct CatalogIdentifierProfileResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogIdentifierResolutionProfile> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
// Heap-free borrowed decode/size/Into encode, including first use. The caller
// owns immutable profile_name backing for the lifetime of a decoded view.
std::optional<std::size_t> CatalogIdentifierProfileEncodedBytes(const CatalogIdentifierResolutionProfile&);
CatalogValueEncodeIntoResult EncodeCatalogIdentifierProfileInto(const CatalogIdentifierResolutionProfile&,std::span<byte>);
CatalogValueEncodeResult EncodeCatalogIdentifierProfile(const CatalogIdentifierResolutionProfile&);
CatalogIdentifierProfileResult DecodeCatalogIdentifierProfile(std::string_view);
bool IsCatalogIdentifierProfilePayload(std::string_view);
bool CatalogIdentifierProfileMatchesHeader(const CatalogTypedRecordView&);
bool CatalogIdentifierProfileMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogIdentifierProfileMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&);
bool CatalogIdentifierProfilePreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog
