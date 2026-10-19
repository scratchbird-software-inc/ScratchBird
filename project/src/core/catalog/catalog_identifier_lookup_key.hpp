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
