// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CATALOG_LANGUAGE_CONSTANTS (owner 2026-11-11, Q53 = 1).
// A human language is a coding-time constant: one fixed UUID per supported
// language, permanent (generated once, 2026-11-11; never regenerated), added
// to as releases support more languages. Inside the engine a language is only
// this UUID: name rows carry it, the name trees key on it, comparisons compare
// it. A language's human-readable tag belongs in the names table.
//
// CatalogLanguageForTag is the ADAPTER at the engine boundary while callers
// (the create request, sessions, the parser wire) still hand over a text tag;
// it is the one place a tag becomes a language, and it refuses a tag with no
// constant (never a default). It goes once the parser sends the UUID.
#include "runtime_platform.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
using scratchbird::core::platform::byte;
using scratchbird::core::platform::Uuid;

// en: 01a1cd40-1989-7378-a9dc-20a683dd17e0
inline constexpr Uuid kCatalogLanguage_en{std::array<byte, 16>{0x01,0xa1,0xcd,0x40,0x19,0x89,0x73,0x78,0xa9,0xdc,0x20,0xa6,0x83,0xdd,0x17,0xe0}};
// en-US: 01a1cd40-198b-7fd3-a076-c136830d6c23
inline constexpr Uuid kCatalogLanguage_en_US{std::array<byte, 16>{0x01,0xa1,0xcd,0x40,0x19,0x8b,0x7f,0xd3,0xa0,0x76,0xc1,0x36,0x83,0x0d,0x6c,0x23}};
// en-CA: 01a1cd40-198e-7cfa-b3b5-6914453c35b7
inline constexpr Uuid kCatalogLanguage_en_CA{std::array<byte, 16>{0x01,0xa1,0xcd,0x40,0x19,0x8e,0x7c,0xfa,0xb3,0xb5,0x69,0x14,0x45,0x3c,0x35,0xb7}};
// fr-CA: 01a1cd40-1990-7f88-b42d-9afadfb5098f
inline constexpr Uuid kCatalogLanguage_fr_CA{std::array<byte, 16>{0x01,0xa1,0xcd,0x40,0x19,0x90,0x7f,0x88,0xb4,0x2d,0x9a,0xfa,0xdf,0xb5,0x09,0x8f}};
// de-DE: 01a1cd40-1992-79ad-86cc-25f1686dcdd0
inline constexpr Uuid kCatalogLanguage_de_DE{std::array<byte, 16>{0x01,0xa1,0xcd,0x40,0x19,0x92,0x79,0xad,0x86,0xcc,0x25,0xf1,0x68,0x6d,0xcd,0xd0}};

struct CatalogLanguage {
  Uuid uuid;
  std::string_view tag;  // the boundary adapter's spelling only
};
inline constexpr std::array<CatalogLanguage, 5> kCatalogLanguages{{
    CatalogLanguage{kCatalogLanguage_en, "en"},
    CatalogLanguage{kCatalogLanguage_en_US, "en-US"},
    CatalogLanguage{kCatalogLanguage_en_CA, "en-CA"},
    CatalogLanguage{kCatalogLanguage_fr_CA, "fr-CA"},
    CatalogLanguage{kCatalogLanguage_de_DE, "de-DE"},
}};

// A language as the typed identity name rows carry (UuidKind::object: a
// coding-time constant, not a catalog row; no verifier looks it up as one).
inline constexpr scratchbird::core::platform::TypedUuid CatalogLanguageIdentity(const Uuid& language) noexcept {
  return {scratchbird::core::platform::UuidKind::object, language};
}
// Whether `uuid` is one of the languages above.
bool IsKnownCatalogLanguage(const Uuid& uuid) noexcept;
// Boundary adapter: the language a text tag names, compared ASCII
// case-insensitively (language tags are case-insensitive, so "EN-us" is
// "en-US"); nullopt for a tag with no constant.
std::optional<Uuid> CatalogLanguageForTag(std::string_view tag) noexcept;
}  // namespace scratchbird::core::catalog
