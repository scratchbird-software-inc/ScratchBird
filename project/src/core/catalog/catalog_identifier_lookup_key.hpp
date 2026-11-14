// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_profile.hpp"
#include "catalog_name_record_codec.hpp"

namespace scratchbird::core::catalog {
enum class CatalogIdentifierKeyError {
  none,invalid_profile,invalid_text,invalid_quote_mode,identifier_too_long,
  resource_admission_required,dialect_admission_required,invalid_backing,
  size_limit,key_mismatch,display_mismatch
};
struct CatalogIdentifierKeyRequest {
  const CatalogIdentifierResolutionProfile* profile=nullptr;
  std::string_view text;
  bool was_quoted=false;
  CatalogNameQuoteStyle quote_style=CatalogNameQuoteStyle::none;
  bool requires_exact_match=false;
};
struct CatalogIdentifierKeyResult {
  CatalogIdentifierKeyError error=CatalogIdentifierKeyError::invalid_profile;
  std::span<const byte> normalized,exact;
  bool ok()const noexcept{return error==CatalogIdentifierKeyError::none;}
};
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_TEXT_RULE (owner Q122 = 1, 2026-11-14).
// The text rule every catalog name obeys whatever its identifier profile:
// non-empty, at most kCatalogIdentifierTextMaximumBytes, canonical UTF-8
// (ValidateCanonicalUtf8) and no U+0000. Pure, allocation-free. invalid_text
// on violation; on success *scalar_count (if given) is the scalar count the
// profile's length rule uses. DeriveCatalogIdentifierLookupKeysInto and
// CheckCatalogIdentifierLookupKeys apply exactly this, after their profile
// checks; the profile's own rules (maximum length, quoting) need the profile.
inline constexpr std::size_t kCatalogIdentifierTextMaximumBytes=131040;
CatalogIdentifierKeyError AdmitCatalogIdentifierText(std::string_view text,u64* scalar_count=nullptr) noexcept;
// Semantic primitive for explicit exact/ASCII fields. The caller must supply
// an actually admitted profile and immutable input; no label selects behavior.
// Resource/platform/dialect-dependent operations require their owning admission.
// Output is two complete UTF-8 lanes, with no allocation or partial writes.
CatalogIdentifierKeyResult DeriveCatalogIdentifierLookupKeysInto(
    const CatalogIdentifierKeyRequest&,std::span<byte>) noexcept;
// Allocation-free comparison of stored lanes and display text. Full-path
// accelerators, collision policy, source admission and authorization are separate.
CatalogIdentifierKeyError CheckCatalogIdentifierLookupKeys(
    const CatalogIdentifierResolutionProfile&,const CatalogNameEntryView&) noexcept;
} // namespace scratchbird::core::catalog
