// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_profile.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_identifier_text_rule.hpp"

namespace scratchbird::core::catalog {
// CatalogIdentifierKeyError is defined in catalog_identifier_text_rule.hpp
// (Q128, 2026-11-14: moved there unchanged with AdmitCatalogIdentifierText).
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
// AdmitCatalogIdentifierText and kCatalogIdentifierTextMaximumBytes live in
// catalog_identifier_text_rule.hpp (header-only since Q128 = 1, 2026-11-14,
// so the SBLR create descriptor codec applies the same copy).
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE (owner Q127 = 3,
// 2026-11-14). The identifier profile's maximum-length rule, the one copy:
// a maximum of 0 is no limit (durable-format tag 12); otherwise the text's
// length in the profile's unit (encoded octets, or the scalar count from
// AdmitCatalogIdentifierText) must not exceed it. identifier_too_long when
// it does. Admit (key derivation) and native CREATE DATABASE (before its
// file is opened, against the pack's profile) both call it.
CatalogIdentifierKeyError AdmitCatalogIdentifierProfileLength(std::string_view text,u64 scalar_count,
    u64 maximum_identifier_length,IdentifierLengthUnit length_unit) noexcept;
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
