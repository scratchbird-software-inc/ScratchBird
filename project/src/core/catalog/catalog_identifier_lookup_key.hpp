// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_profile.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_identifier_text_rule.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
// SEARCH_KEY: SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1, 2026-11-14: "raw name
// only"; plan 35 S1). A name entry row (schema 327682 version 4) stores its
// raw text and the lane inputs (was_quoted, quote_style,
// requires_exact_match, the identifier profile it was admitted under and the
// resource epoch), never a folded copy: the display name, the normalized and
// exact lookup keys and the full path (fields 16, 20-23 of version 3) are
// derived at read under the bound profile. (Was: stored, version 3, and
// compared with a fresh derivation by CheckCatalogIdentifierLookupKeys.)
// Admitted folds are ASCII only (Q182 = 1), one output byte per input byte,
// so a derived lane is the raw text and the fold applied to each byte: a
// view, allocation-free, compared and escaped in place.
struct CatalogNameDerivedText {
  std::string_view text;  // the entry's raw_name_text (borrowed)
  IdentifierFoldMode fold = IdentifierFoldMode::none;  // none, upper_ascii or lower_ascii
  std::size_t size() const noexcept { return text.size(); }
  bool empty() const noexcept { return text.empty(); }
  byte operator[](std::size_t at) const noexcept {
    const auto b = static_cast<byte>(text[at]);
    if (fold == IdentifierFoldMode::upper_ascii && b >= 'a' && b <= 'z') return static_cast<byte>(b - ('a' - 'A'));
    if (fold == IdentifierFoldMode::lower_ascii && b >= 'A' && b <= 'Z') return static_cast<byte>(b + ('a' - 'A'));
    return b;
  }
  std::vector<byte> Bytes() const;
  std::string String() const;
  bool Equals(std::span<const byte> bytes) const noexcept;
};
// Unsigned byte order, as std::lexicographical_compare over the derived bytes.
int CompareCatalogNameDerivedText(const CatalogNameDerivedText& x, const CatalogNameDerivedText& y) noexcept;
struct CatalogNameEntryKeys {
  CatalogIdentifierKeyError error = CatalogIdentifierKeyError::invalid_profile;
  CatalogNameDerivedText normalized, exact;
  CatalogNameDerivedText own;      // exact when requires_exact_match, else normalized: the key trees and collisions use
  CatalogNameDerivedText display;  // preserve_user_text: the raw text; store_folded_display: the own lane
  bool ok() const noexcept { return error == CatalogIdentifierKeyError::none; }
};
// The entry's lanes under `profile`: refused as CheckCatalogIdentifierLookupKeys
// refuses (the profile must admit the text and quoting, be the entry's own
// identifier profile, and the entry carries no case-fold or quoted-identifier
// supplement). donor_projection display is refused by admission
// (resource_admission_required).
CatalogNameEntryKeys DeriveCatalogNameEntryKeys(const CatalogIdentifierResolutionProfile& profile,
                                               const CatalogNameEntryView& entry) noexcept;
// Allocation-free admission of an entry under its profile: the error
// DeriveCatalogNameEntryKeys gives. Collision policy, source admission and
// authorization are separate. (Was: comparison of the stored lanes and display
// text with a fresh derivation, key_mismatch / display_mismatch; a version 4
// row stores neither, SB_NATIVE_NAME_ROW_V4.)
CatalogIdentifierKeyError CheckCatalogIdentifierLookupKeys(
    const CatalogIdentifierResolutionProfile&,const CatalogNameEntryView&) noexcept;
} // namespace scratchbird::core::catalog
