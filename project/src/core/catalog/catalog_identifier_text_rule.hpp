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
// Retired (owner Q217 = yes, 2026-11-15; plan 35 follow-up): key_mismatch
// and display_mismatch, the comparison of a version 3 row's stored lanes and
// display with a fresh derivation (SB_NATIVE_NAME_ROW_V4 stores neither). The
// values were never persisted or sent; the enum is renumbered.
enum class CatalogIdentifierKeyError {
  none,invalid_profile,invalid_text,invalid_quote_mode,identifier_too_long,
  resource_admission_required,dialect_admission_required,invalid_backing,
  size_limit,
  // Part B S6 (owner 2026-11-23): the text rule's own refusals, one code each (appended, not persisted or sent).
  // invalid_text is now only the NULL name (a null pointer with a nonzero size).
  text_too_long,text_invalid_utf8,text_contains_nul
};
// SB_PART_B_S6_NAME_ADMISSION (owner 2026-11-22/23, Part B S6; was: non-empty, at most 131040 bytes). The ENGINE's
// whole admission rule for a catalog name, which is DATA: stored as given, no normalization, no fold. The empty name ""
// IS a name (owner: "empty '' allowed but not null"). Refused, each with its own code, in this order:
//   invalid_text        the NULL name (a null pointer with a nonzero size; std::string and string_view cannot carry
//                       NULL as distinct from "", so the wire/ABI layers that can say "absent" refuse it themselves)
//   text_too_long       more than kCatalogNameCapacityBytes bytes
//   text_invalid_utf8   not canonical UTF-8 (ValidateCanonicalUtf8: shortest form, no surrogates, <= U+10FFFF)
//   text_contains_nul   a U+0000 byte
// Pure, allocation-free. On success *scalar_count (if given) is the scalar count (kept for the retained profile-length
// helper; the engine itself no longer applies a profile length). DeriveCatalogIdentifierLookupKeysInto and
// CheckCatalogIdentifierLookupKeys apply exactly this, after their profile checks.
// ONE capacity constant: the engine's name capacity is 4096 BYTES of UTF-8 (owner 2026-11-23: "yes" to "the ENGINE's
// name capacity is 4096 BYTES (UTF-8) and SBsql advertises 128 characters (parser-side)"). It supersedes the earlier
// 131040-byte text maximum and the pack's 512-scalar maximum_identifier_length as ENGINE limits; the profile row's value
// is stored data (S7 removes profile rows) and is no longer enforced on a native write. A page size can still bound
// what a name TREE CELL carries (a quarter page; name_key_exceeds_page_capacity): that is a storage bound, a different code.
inline constexpr std::size_t kCatalogNameCapacityBytes=4096;
inline constexpr std::size_t kCatalogIdentifierTextMaximumBytes=kCatalogNameCapacityBytes;  // the old spelling, derived
inline CatalogIdentifierKeyError AdmitCatalogIdentifierText(std::string_view text,
                                                            std::uint64_t* scalar_count=nullptr) noexcept {
  std::uint64_t scalars=0;
  if(scalar_count)*scalar_count=0;
  if(text.data()==nullptr&&!text.empty())return CatalogIdentifierKeyError::invalid_text;
  if(text.size()>kCatalogNameCapacityBytes)return CatalogIdentifierKeyError::text_too_long;
  if(!datatypes::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),&scalars))
    return CatalogIdentifierKeyError::text_invalid_utf8;
  if(text.find('\0')!=text.npos)return CatalogIdentifierKeyError::text_contains_nul;
  if(scalar_count)*scalar_count=scalars;
  return CatalogIdentifierKeyError::none;
}
} // namespace scratchbird::core::catalog
