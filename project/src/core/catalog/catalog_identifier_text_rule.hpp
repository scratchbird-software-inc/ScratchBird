// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_TEXT_RULE (owner Q122 = 1, Q128 = 1,
// 2026-11-14). The text rule every catalog name obeys whatever its
// identifier profile, header-only so that wire codecs outside the catalog
// library (the SBLR create descriptor codec, shared with parser producers,
// Q128) apply the same single copy without linking the catalog library.
// Depends only on canonical_utf8.hpp (header-only).
// (Was: declared in catalog_identifier_lookup_key.hpp and defined in
// catalog_identifier_lookup_key.cpp, with the enum below; moved unchanged.)
#include "../datatypes/canonical_utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scratchbird::core::catalog {
enum class CatalogIdentifierKeyError {
  none,invalid_profile,invalid_text,invalid_quote_mode,identifier_too_long,
  resource_admission_required,dialect_admission_required,invalid_backing,
  size_limit,key_mismatch,display_mismatch
};
// Non-empty, at most kCatalogIdentifierTextMaximumBytes, canonical UTF-8
// (ValidateCanonicalUtf8) and no U+0000. Pure, allocation-free. invalid_text
// on violation; on success *scalar_count (if given) is the scalar count the
// profile's length rule uses. DeriveCatalogIdentifierLookupKeysInto and
// CheckCatalogIdentifierLookupKeys apply exactly this, after their profile
// checks; the profile's own rules (maximum length, quoting) need the profile.
inline constexpr std::size_t kCatalogIdentifierTextMaximumBytes=131040;
inline CatalogIdentifierKeyError AdmitCatalogIdentifierText(std::string_view text,
                                                            std::uint64_t* scalar_count=nullptr) noexcept {
  std::uint64_t scalars=0;
  if(scalar_count)*scalar_count=0;
  if(text.empty()||text.size()>kCatalogIdentifierTextMaximumBytes||!datatypes::ValidateCanonicalUtf8(
      reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),&scalars)||text.find('\0')!=text.npos)
    return CatalogIdentifierKeyError::invalid_text;
  if(scalar_count)*scalar_count=scalars;
  return CatalogIdentifierKeyError::none;
}
} // namespace scratchbird::core::catalog
